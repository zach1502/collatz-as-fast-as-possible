// Part of units.ts: the unit row, a unit's submitted runs, the state they give
// the unit, a run as an alert names it, and SQL that the writers of units
// share: the batch guard, the frontier pointer, the condition of a unit that
// waits for a re-check and the insert of an accepted unit's records.

import { Db, OWNER_USER } from "../http";
import { Block } from "../result";

export interface UnitRow extends Block {
    id: number;
    campaign_id: number;
    len: number;
    state: string;
    disputed: number;
    wanted: number;
    assigned: number;
    results: number;
    checks: number;
    digest: string | null;
    created_at: number;
    accepted_at: number | null;
    recheck: string | null;
    recheck_kind: string | null;
    special_at: number | null;
    draw_at: number | null;
    rev: number;
}

/** A submitted run, as the derivation and the alerts see it. */
export interface SubmittedRun {
    id: number;
    host: string;
    role: string;
    backend: string;
    kind: string | null;
    digest: string;
    result_json: string | null;
}

export const SUBMITTED_RUN_COLS = "id, host, role, backend, kind, digest, result_json";

/** A run named in an alert: {user, host, role, kind, backend, digest}; user is always OWNER_USER. */
export interface AlertRun {
    user: string;
    host: string;
    role: string;
    kind: string | null;
    backend: string;
    digest: string;
}

export interface Derived {
    state: "accepted" | "conflict";
    /** The accepted digest; null unless accepted. */
    digest: string | null;
    /** units.checks: 1 when accepted, else 0. */
    checks: number;
    wanted: number;
    /** 1 when the runs hold more than one digest. */
    disputed: number;
}

/**
 * The state that a unit's submitted runs (at least one) give it. Runs with one
 * digest accept the unit with it (quorum 1): one result accepts a block, and a
 * re-check that agrees adds no vote, so units.checks is 1. Two digests come
 * from a re-check that disagreed: a conflict, which wants one more run and
 * waits for the owner's decision (admin_units.ts).
 */
export function derive(runs: SubmittedRun[]): Derived {
    const digests = new Set(runs.map((r) => r.digest));
    const [only] = digests;
    if (digests.size === 1 && only !== undefined) {
        return { state: "accepted", digest: only, checks: 1, wanted: runs.length, disputed: 0 };
    }
    return { state: "conflict", digest: null, checks: 0, wanted: runs.length + 1, disputed: 1 };
}

/**
 * Fails the batch (rolling it back) unless the statement before it changed
 * exactly one row. json() on malformed text raises an SQL error; D1 runs a
 * batch as one transaction on one connection, so changes() refers to the
 * previous statement (trigger writes are not counted).
 */
export const GUARD = "SELECT CASE WHEN changes() = 1 THEN 1 ELSE json('concurrent-update') END";
export const isGuardFailure = (e: unknown): boolean => e instanceof Error && /malformed JSON/i.test(e.message);

/**
 * The end of the contiguous run of accepted units that starts at slot `start`
 * (an SQL expression without outer references), followed unit by unit
 * through the units_pos index.
 */
const chainSql = (campaign: string, start: string): string =>
    `(WITH RECURSIVE chain(p) AS (
          SELECT ${start}
          UNION ALL
          SELECT u.pos + u.len FROM units u JOIN chain ON u.campaign_id = ${campaign} AND u.pos = chain.p
          WHERE u.state = 'accepted')
      SELECT MAX(p) FROM chain)`;

/**
 * SET expression for campaigns.frontier_pos after unit at slot ?pos changed:
 * a unit that became accepted at the frontier moves it along the chain; a
 * unit that left the accepted state below it moves it back to that unit. Any
 * other change keeps it.
 */
export const frontierSql = (campaign: string, pos: string, accepted: string, left: string): string =>
    `CASE WHEN ${accepted} = 1 AND frontier_pos = ${pos} THEN ${chainSql(campaign, pos)}
          WHEN ${left} = 1 THEN MIN(frontier_pos, ${pos})
          ELSE frontier_pos END`;

/**
 * A unit (columns prefixed with `t`) that waits for a re-check, or a conflict
 * that waits for the owner: the WHERE of the units_special, units_drawn and
 * units_draw indexes, which a query must repeat to use them.
 */
export const waitingSql = (t = ""): string => `((${t}state = 'accepted' AND ${t}recheck = 'pending') OR ${t}state = 'conflict')`;

/**
 * The insert of the records `hits` of accepted unit `unitId` (users 'owner',
 * found at `now`), one statement however many there are, so that a batch does
 * not grow with them; none without hits. A number another unit of the
 * campaign already holds stays with it.
 */
export function recordsInsert(db: Db, campaignId: number, unitId: number, hits: Array<{ n: string; peak: string }>, now: number): D1PreparedStatement[] {
    if (hits.length === 0) {
        return [];
    }
    return [
        db.prep(
            `INSERT INTO records (campaign_id, n, peak, unit_id, users, found_at)
             SELECT ?1, json_extract(value, '$[0]'), json_extract(value, '$[1]'), ?2, ?3, ?4 FROM json_each(?5) WHERE true
             ON CONFLICT (campaign_id, n) DO NOTHING`,
            campaignId,
            unitId,
            OWNER_USER,
            now,
            JSON.stringify(hits.map((h) => [h.n, h.peak])),
        ),
    ];
}
