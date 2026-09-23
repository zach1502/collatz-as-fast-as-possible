// GET /api/ledger, /api/ledger.csv and /api/ledger/head (ledger.ts describes
// the ledger), and ledgerSummary, the ledger figures of /api/stats.

import { Db, Env, HttpError, intVar, ledgerPageLimit } from "./http";
import { LEDGER_GENESIS, LINE_FIELDS, readHead } from "./ledger_chain";

/**
 * Entries per page of /api/ledger and /api/ledger.csv. Pages start at seq 1,
 * 1001, 2001, ..., so each kind has one answer (and one cache entry) per page
 * however the parameters are written.
 */
const LEDGER_PAGE = 1000;
/** The fields of a published entry, in order: the JSON keys and the CSV header. */
const LEDGER_COLUMNS = ["format", ...LINE_FIELDS, "hash"] as const;

const COLS = `format, seq, campaign_id AS campaign, at, kind, start, bits, "end", result, peak, peak_n, seconds, machine, backend,
              version, schema, who, votes, prev, hash`;

const csvCell = (v: unknown): string => {
    const s = String(v);
    return /[",\r\n]/.test(s) ? `"${s.replaceAll('"', '""')}"` : s;
};

const pageCache = new Map<string, { at: number; body: string }>();
/** Answers the isolate keeps; a full JSON page is about 600 KB. */
const PAGE_CACHE_ENTRIES = 16;

function headers(ttl: number, type: string, cache: string): Record<string, string> {
    return {
        "content-type": type,
        "cache-control": ttl > 0 ? `public, max-age=${ttl}` : "no-store",
        "access-control-allow-origin": "*",
        "x-content-type-options": "nosniff",
        "x-ledger-cache": cache,
        ...(type.startsWith("text/csv") ? { "content-disposition": 'attachment; filename="ledger.csv"' } : {}),
    };
}

/** A non-negative integer query parameter of at most 15 digits, or `fallback` when it is absent or empty. */
function param(url: URL, name: string, fallback: number): number {
    const v = url.searchParams.get(name);
    if (v === null || v === "") {
        return fallback;
    }
    if (!/^[0-9]{1,15}$/.test(v)) {
        throw new HttpError(400, `${name} must be a non-negative integer`);
    }
    return Number(v);
}

/**
 * GET /api/ledger?from=SEQ: a JSON array of the page of entries that holds
 * seq SEQ, ascending: the 1000 entries from seq 1 + 1000k, where SEQ is
 * rounded down to such a start (default 1). `limit` may be given, as 1000
 * only. /api/ledger.csv takes the same parameters and gives a header row,
 * then one row per entry. /api/ledger/head gives {seq, hash, at}: seq 0, 64
 * zeros and at null while the ledger is empty. A page is read by seq through
 * the primary key, so it reads at most 1000 rows, and a page read from D1
 * counts against the address's page budget (ledgerPageLimit). Answers are
 * cached STATS_CACHE_SECONDS (default 60) in the isolate and in the Cache
 * API, as /api/stats is; there is no D1 copy, since every page would cost a
 * row write. TEST_MODE only: X-Test-Stats-Cache-Seconds sets the cache time.
 */
export async function ledger(db: Db, env: Env, request: Request, url: URL): Promise<Response> {
    const what = url.pathname === "/api/ledger/head" ? "head" : url.pathname === "/api/ledger.csv" ? "csv" : "json";
    const from = what === "head" ? 1 : Math.max(1, param(url, "from", 1));
    if (what !== "head" && param(url, "limit", LEDGER_PAGE) !== LEDGER_PAGE) {
        throw new HttpError(400, `limit must be ${LEDGER_PAGE}: the ledger is served in pages of ${LEDGER_PAGE} entries from seq 1, ${LEDGER_PAGE + 1}, ...`);
    }
    const start = from - ((from - 1) % LEDGER_PAGE);
    const test = env.TEST_MODE === "1" ? request.headers.get("x-test-stats-cache-seconds") : null;
    const ttl = Math.max(0, Math.min(300, Math.floor(intVar(test ?? env.STATS_CACHE_SECONDS, 60))));
    const type = what === "csv" ? "text/csv; charset=utf-8" : "application/json; charset=utf-8";
    const key = what === "head" ? "head" : `${what}:${start}`;
    const nowMs = Date.now();
    const cached = pageCache.get(key);
    if (ttl > 0 && cached !== undefined && nowMs - cached.at < ttl * 1000) {
        return new Response(cached.body, { headers: headers(ttl, type, "isolate") });
    }
    // The Cache API keys on a URL; this one only names the entry.
    const cacheKey = new Request(`${url.origin}/api/ledger?page=${key}`);
    if (ttl > 0) {
        const hit = await caches.default.match(cacheKey).catch(() => undefined);
        if (hit !== undefined) {
            return new Response(hit.body, { headers: headers(ttl, type, "edge") });
        }
    }
    let body: string;
    if (what === "head") {
        const h = await readHead(db);
        body = JSON.stringify({ seq: h.seq, hash: h.hash, at: h.seq === 0 ? null : h.at });
    } else {
        await ledgerPageLimit(env, request);
        const rows = await db.all<Record<string, unknown>>(`SELECT ${COLS} FROM ledger WHERE seq >= ?1 ORDER BY seq LIMIT ?2`, start, LEDGER_PAGE);
        if (what === "json") {
            body = JSON.stringify(rows.map((r) => Object.fromEntries(LEDGER_COLUMNS.map((k) => [k, r[k]]))));
        } else {
            const lines = [LEDGER_COLUMNS.join(","), ...rows.map((r) => LEDGER_COLUMNS.map((k) => csvCell(r[k])).join(","))];
            body = lines.map((l) => `${l}\r\n`).join("");
        }
    }
    if (ttl > 0) {
        if (pageCache.size >= PAGE_CACHE_ENTRIES) {
            const oldest = pageCache.keys().next().value;
            if (oldest !== undefined) {
                pageCache.delete(oldest);
            }
        }
        pageCache.set(key, { at: nowMs, body });
        await caches.default.put(cacheKey, new Response(body, { headers: headers(ttl, type, "edge") })).catch(() => undefined);
    }
    return new Response(body, { headers: headers(ttl, type, "miss") });
}

export function ledgerSummary(h: { seq: number; hash: string } | undefined): { entries: number; head: string; head_full: string } {
    const hash = h?.hash ?? LEDGER_GENESIS;
    return { entries: h?.seq ?? 0, head: hash.slice(0, 16), head_full: hash };
}
