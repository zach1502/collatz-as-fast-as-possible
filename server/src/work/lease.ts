// Part of work.ts: the lease of a regular run and the choice fields of a fetch
// body, which fetchWork (fetch.ts) uses, and POST /api/release, which hands a
// lease back.

import { JsonObject } from "../json";
import { Db, HttpError, OWNER_USER, checkHost, json } from "../http";
import { Campaign } from "../result";
import { intField, leaseIdField } from "../units";

/**
 * Leases open unit `unitId` to the owner's machine `host` as a regular run.
 * The unit must have a free slot, and the owner's machines must hold no live
 * or submitted run of it and never have run it as a re-check. A released or
 * expired regular run of it is taken back. With `renewOnly` (prefer_unit)
 * only that is tried, so prefer_unit cannot pick a unit the machines never
 * held. Each statement checks every condition, so it cannot oversubscribe a
 * unit. The runs_assign_* triggers keep units.assigned. Returns the id of
 * the leased run (the lease id), or null.
 */
export async function leaseRegular(
    db: Db,
    c: Campaign,
    unitId: number,
    host: string,
    backend: string,
    now: number,
    renewOnly: boolean,
): Promise<number | null> {
    const cond = `u.id = ?1 AND u.campaign_id = ?6 AND u.state = 'open' AND u.assigned < u.wanted
        AND NOT EXISTS (SELECT 1 FROM runs x WHERE x.unit_id = u.id AND x.user = ?2
                        AND (x.state IN ('leased', 'submitted') OR x.role <> 'regular'))`;
    const args = [unitId, OWNER_USER, host, now, now + c.lease_seconds, c.id, backend];
    // RETURNING, not meta.changes: D1 counts the trigger's write in changes.
    const renewed = await db.all<{ id: number }>(
        `UPDATE runs SET state = 'leased', host = ?3, leased_at = ?4, expires_at = ?5, backend = ?7
         WHERE id = (SELECT r.id FROM runs r JOIN units u ON u.id = r.unit_id
                     WHERE r.unit_id = ?1 AND r.user = ?2 AND r.role = 'regular' AND r.state IN ('released', 'expired') AND ${cond}
                     ORDER BY r.id DESC LIMIT 1)
         RETURNING id`,
        ...args,
    );
    if (renewed.length === 1 || renewOnly) {
        return renewed[0]?.id ?? null;
    }
    const leased = await db.all<{ id: number }>(
        `INSERT INTO runs (unit_id, user, host, role, state, backend, leased_at, expires_at)
         SELECT u.id, ?2, ?3, 'regular', 'leased', ?7, ?4, ?5 FROM units u
         WHERE ${cond} AND NOT EXISTS (SELECT 1 FROM runs x WHERE x.unit_id = u.id AND x.user = ?2)
         RETURNING id`,
        ...args,
    );
    return leased[0]?.id ?? null;
}

export function stringChoice(b: JsonObject, key: string, choices: string[], fallback: string): string {
    const v = b[key];
    if (v === undefined) {
        return fallback;
    }
    if (typeof v !== "string" || !choices.includes(v)) {
        throw new HttpError(400, `${key} must be one of ${choices.join(", ")}`);
    }
    return v;
}

/**
 * POST /api/release {token, host, unit_id, lease_id}: hands back lease
 * `lease_id` of the unit, while it is this machine's and live or expired. A
 * lease that another machine took over in the meantime stays with it.
 */
export async function release(db: Db, b: JsonObject): Promise<Response> {
    const host = checkHost(b["host"]);
    const unitId = intField(b, "unit_id");
    const leaseId = leaseIdField(b);
    const released = await db.all<{ id: number }>(
        `UPDATE runs SET state = 'released'
         WHERE id = ?1 AND unit_id = ?2 AND user = ?3 AND host = ?4 AND state IN ('leased', 'expired') RETURNING id`,
        leaseId,
        unitId,
        OWNER_USER,
        host,
    );
    return json(200, { ok: true, released: released.length > 0 });
}
