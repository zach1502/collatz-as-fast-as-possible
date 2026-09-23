// GET /api/stats: the handler and the caches of its answer.

import { Db, Env, HttpError, intVar, nowSeconds } from "../http";
import { build } from "./document";

const statsCache = new Map<string, { at: number; body: string }>();

function headers(ttl: number, cache: string): Record<string, string> {
    return {
        "content-type": "application/json; charset=utf-8",
        "cache-control": ttl > 0 ? `public, max-age=${ttl}` : "no-store",
        "access-control-allow-origin": "*",
        "x-content-type-options": "nosniff",
        "x-stats-cache": cache,
    };
}

/**
 * GET /api/stats[?campaign=ID]: what the page shows (document.ts), with the
 * machines (host ids). Within the cache time an answer comes from the
 * isolate's copy, the Cache API, or the stats_cache row (one row read), in
 * that order; otherwise it is built and stored in all three.
 * TEST_MODE only: X-Test-Now replaces the clock, X-Test-Stats-Cache-Seconds
 * the cache time, and X-Test-Stats-Cache-Layer: edge or d1 skips the layers
 * before that one.
 */
export async function stats(db: Db, env: Env, request: Request, url: URL): Promise<Response> {
    const raw = url.searchParams.get("campaign") ?? "";
    if (raw !== "" && !/^[0-9]{1,15}$/.test(raw)) {
        throw new HttpError(400, "campaign must be an integer id");
    }
    // The id as a number, so that 1, 01 and 001 share one answer and one
    // cache entry (and one stats_cache row).
    const which = raw === "" ? "" : String(Number(raw));
    const test = env.TEST_MODE === "1";
    const header = (name: string): string | null => (test ? request.headers.get(name) : null);
    const ttl = Math.max(0, Math.min(300, Math.floor(intVar(header("x-test-stats-cache-seconds") ?? env.STATS_CACHE_SECONDS, 60))));
    const layer = header("x-test-stats-cache-layer");
    const nowMs = Date.now();
    const key = `stats:${which}`;
    const cached = statsCache.get(key);
    if (ttl > 0 && layer === null && cached !== undefined && nowMs - cached.at < ttl * 1000) {
        return new Response(cached.body, { headers: headers(ttl, "isolate") });
    }
    // The Cache API keys on a URL; this one only names the entry.
    const cacheKey = new Request(`${url.origin}/api/stats?entry=${which}`);
    if (ttl > 0 && layer !== "d1") {
        const hit = await caches.default.match(cacheKey).catch(() => undefined);
        if (hit !== undefined) {
            return new Response(hit.body, { headers: headers(ttl, "edge") });
        }
    }
    if (ttl > 0) {
        const row = await db.first<{ at: number; body: string }>("SELECT at, body FROM stats_cache WHERE key = ?1", key);
        if (row !== null && nowMs - row.at * 1000 < ttl * 1000) {
            statsCache.set(key, { at: row.at * 1000, body: row.body });
            return new Response(row.body, { headers: headers(ttl, "d1") });
        }
    }
    const testNow = Number(header("x-test-now"));
    const now = Number.isSafeInteger(testNow) && testNow > 0 ? testNow : nowSeconds();
    const body = JSON.stringify(await build(db, env, which, now));
    if (ttl > 0) {
        statsCache.set(key, { at: nowMs, body });
        await db.run(
            "INSERT INTO stats_cache (key, at, body) VALUES (?1, ?2, ?3) ON CONFLICT (key) DO UPDATE SET at = excluded.at, body = excluded.body",
            key,
            Math.floor(nowMs / 1000),
            body,
        );
        await caches.default.put(cacheKey, new Response(body, { headers: headers(ttl, "edge") })).catch(() => undefined);
    }
    return new Response(body, { headers: headers(ttl, "miss") });
}
