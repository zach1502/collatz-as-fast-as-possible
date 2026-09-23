// Owner endpoints under /api/admin/ (server/scripts/admin.mjs calls them):
// list and resolve alerts (re-checks that disagreed, audits with failures),
// accept a unit in conflict with one of its results or start it over, and
// withdraw an accepted unit (admin_units.ts). Each request needs
// `Authorization: Bearer <ADMIN_SECRET>`.

import { JsonNumber, JsonObject } from "./json";
import { Db, Env, HttpError, json, nowSeconds, sameString } from "./http";
import { sha256Hex } from "./result";
import { noteOf } from "./admin_units";

export { decide } from "./admin_units";

/** What the unit of an open alert waits for while it is in conflict. */
const NEEDS_DECISION = "needs an owner decision";

/**
 * Checks `Authorization: Bearer <ADMIN_SECRET>`. Surrounding whitespace of the
 * secret is ignored, as for WORKER_TOKEN (auth.ts), in case `wrangler secret
 * put` kept a trailing newline of the file it was given.
 */
export async function requireAdmin(request: Request, env: Env): Promise<void> {
    const secret = env.ADMIN_SECRET?.trim() ?? "";
    if (secret.length < 32) {
        throw new HttpError(503, "the admin API is off; set ADMIN_SECRET (at least 32 characters) to use it");
    }
    const m = /^Bearer (\S+)$/.exec(request.headers.get("authorization") ?? "");
    // Comparing digests keeps the time independent of the secret's length and
    // of where a guess differs.
    if (m === null || !sameString(await sha256Hex(m[1] ?? ""), await sha256Hex(secret))) {
        throw new HttpError(401, "admin authorization required (Authorization: Bearer ADMIN_SECRET)");
    }
}

/** GET /api/admin/alerts[?all=1]: alerts, newest first; unresolved ones only unless all=1. */
export async function alerts(db: Db, url: URL): Promise<Response> {
    const all = url.searchParams.get("all") === "1";
    const rows = await db.all<{
        id: number;
        campaign_id: number;
        unit_id: number;
        kind: string | null;
        fields: string;
        runs: string;
        created_at: number;
        outcome: string | null;
        settled_at: number | null;
        resolved_at: number | null;
        note: string | null;
        pos: number | null;
        bits: number | null;
        state: string | null;
        l0: string | null;
        min_bits: number | null;
    }>(
        `SELECT a.id, a.campaign_id, a.unit_id, a.kind, a.fields, a.runs, a.created_at, a.outcome, a.settled_at, a.resolved_at,
                a.note, u.pos, u.bits, u.state, c.l0, c.min_bits
         FROM alerts a LEFT JOIN units u ON u.id = a.unit_id LEFT JOIN campaigns c ON c.id = a.campaign_id
         WHERE ?1 = 1 OR a.resolved_at IS NULL ORDER BY a.id DESC LIMIT 200`,
        all ? 1 : 0,
    );
    return json(200, {
        alerts: rows.map(({ pos, bits, l0, min_bits, ...r }) => ({
            ...r,
            fields: JSON.parse(r.fields) as string[],
            runs: JSON.parse(r.runs) as unknown[],
            h: pos !== null && bits !== null && l0 !== null && min_bits !== null
                ? ((BigInt(l0) + (BigInt(pos) << BigInt(min_bits))) >> BigInt(bits)).toString()
                : null,
            k: bits,
            needs: r.resolved_at === null && r.state === "conflict" ? NEEDS_DECISION : null,
        })),
    });
}

/** POST /api/admin/alerts/resolve {id, note}: marks an alert as handled by the owner; the unit is not changed. */
export async function resolveAlert(db: Db, b: JsonObject): Promise<Response> {
    const id = b["id"];
    if (!(id instanceof JsonNumber) || !/^[1-9][0-9]{0,14}$/.test(id.raw)) {
        throw new HttpError(400, "id must be an alert id");
    }
    const note = noteOf(b);
    const r = await db.all<{ id: number }>(
        "UPDATE alerts SET resolved_at = ?1, note = ?2 WHERE id = ?3 AND resolved_at IS NULL RETURNING id",
        nowSeconds(),
        note,
        Number(id.raw),
    );
    return json(200, { ok: true, id: Number(id.raw), resolved: r.length === 1 });
}
