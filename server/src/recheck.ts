// Re-checks. An accepted unit drawn at the campaign's recheck_rate waits for
// one more run of the same block on the owner's machines: on another active
// host first, then on the other backend of the same host, then on the same
// host and backend. A worker is not told that a lease is a re-check. A
// re-check that agrees passes; one that disagrees puts the unit in conflict,
// out of the frontier, until the owner decides it (admin_units.ts).

import { Db, Env, OWNER_USER, embargoSeconds, recheckEvery, recheckMaxWait } from "./http";
import { Campaign } from "./result";
import { ACTIVE_SECONDS, waitingSql } from "./units";

/** How a re-check run differs from the unit's earlier runs: another host, the other backend of the same host, or neither. */
type Kind = "host" | "backend" | "same";
const RANK: Record<Kind, number> = { host: 0, backend: 1, same: 2 };

/** What a fetching machine offers: its backend, whether it also runs re-checks on the CPU, and the block size it asked for. */
export interface Offer {
    host: string;
    backend: "gpu" | "cpu";
    recheckCpu: boolean;
    bits: number;
}

interface PriorRun {
    host: string;
    backend: string;
}

export function runKind(prior: PriorRun[], host: string, backend: string): Kind {
    if (!prior.some((r) => r.host === host)) {
        return "host";
    }
    return prior.some((r) => r.host === host && r.backend === backend) ? "same" : "backend";
}

/**
 * The backend a re-check on this machine uses: the CPU when the machine
 * offers it (--recheck-cpu, or a CPU worker) and every earlier run of the unit
 * ran on a GPU, so a unit checked only on GPUs gets a CPU run; else the
 * machine's own backend.
 */
function chooseBackend(prior: PriorRun[], o: { backend: string; recheckCpu: boolean }): "gpu" | "cpu" {
    if (o.backend === "cpu") {
        return "cpu";
    }
    return o.recheckCpu && prior.length > 0 && prior.every((r) => r.backend === "gpu") ? "cpu" : "gpu";
}

/**
 * Whether a re-check lease of `leaseSeconds` taken now ends before the
 * embargo of a unit accepted at `acceptedAt` (EMBARGO_SECONDS), by which the
 * re-check must be submitted.
 */
export const recheckOfferable = (env: Env, acceptedAt: number, now: number, leaseSeconds: number): boolean =>
    now + leaseSeconds <= acceptedAt + embargoSeconds(env);

/** One more regular result of the owner's machines in the campaign since the last re-check lease (RECHECK_EVERY). */
export const countRegularResult = (db: Db, campaignId: number, now: number): D1PreparedStatement =>
    db.prep(
        `INSERT INTO user_stats (campaign_id, user, since_recheck, last_at) VALUES (?1, ?2, 1, ?3)
         ON CONFLICT (campaign_id, user) DO UPDATE SET since_recheck = since_recheck + 1, last_at = MAX(last_at, excluded.last_at)`,
        campaignId,
        OWNER_USER,
        now,
    );

/**
 * Looks at the waiting unit of campaign `c` whose draw_at is oldest and that
 * has had no live re-check lease for lease_seconds (through the units_draw
 * index). A re-check that no new lease could finish before the embargo ends
 * expires; any other waiting unit gets draw_at = now, so that the next fetch
 * looks at the next one.
 */
export async function expireDue(db: Db, env: Env, c: Campaign, now: number): Promise<void> {
    const due = await db.first<{ id: number; state: string; accepted_at: number | null; draw_at: number }>(
        `SELECT u.id, u.state, u.accepted_at, u.draw_at FROM units u
         WHERE u.campaign_id = ?1 AND ${waitingSql("u.")} AND u.draw_at <= ?2
           AND NOT EXISTS (SELECT 1 FROM runs r WHERE r.unit_id = u.id AND r.role <> 'regular' AND r.state = 'leased'
                           AND r.expires_at > ?3)
         ORDER BY u.draw_at LIMIT 1`,
        c.id,
        now - c.lease_seconds,
        now,
    );
    if (due === null) {
        return;
    }
    if (due.state === "accepted" && !recheckOfferable(env, due.accepted_at ?? 0, now, c.lease_seconds)) {
        await db.run(
            `UPDATE units SET recheck = 'expired', special_at = NULL, draw_at = NULL, rev = rev + 1
             WHERE id = ?1 AND recheck = 'pending' AND draw_at = ?2`,
            due.id,
            due.draw_at,
        );
        return;
    }
    await db.run(`UPDATE units SET draw_at = ?1, rev = rev + 1 WHERE id = ?2 AND draw_at = ?3 AND ${waitingSql()}`, now, due.id, due.draw_at);
}

interface Candidate {
    id: number;
    pos: number;
    bits: number;
    accepted_at: number | null;
    special_at: number;
}

interface CandidateRun {
    unit_id: number;
    host: string;
    backend: string;
    state: string;
}

/**
 * Leases a waiting re-check to machine `o.host`, or returns null. Only units
 * no larger than the asked block size are considered, the longest waiting
 * first. A re-check goes out when the owner's machines have had RECHECK_EVERY
 * regular results since the last re-check lease, when it has waited
 * RECHECK_MAX_WAIT_SECONDS, or when `idle` (there is no regular work), and
 * only if its lease ends before the embargo. Until RECHECK_MAX_WAIT_SECONDS it
 * waits for the host that gives the best kind among the owner's active hosts
 * that ask for blocks this large.
 */
export async function tryRecheck(
    db: Db,
    env: Env,
    c: Campaign,
    o: Offer,
    now: number,
    idle: boolean,
): Promise<{ unit: Candidate; backend: string; runId: number } | null> {
    const late = now - recheckMaxWait(env);
    // At most 16 rows, read through the units_special index.
    const cands = await db.all<Candidate>(
        `SELECT id, pos, bits, accepted_at, special_at FROM units
         WHERE campaign_id = ?1 AND ${waitingSql()} AND state = 'accepted' AND bits <= ?2
         ORDER BY special_at LIMIT 16`,
        c.id,
        o.bits,
    );
    if (cands.length === 0) {
        return null;
    }
    let ratioOk = idle;
    if (!ratioOk) {
        const s = await db.first<{ since_recheck: number }>(
            "SELECT since_recheck FROM user_stats WHERE campaign_id = ?1 AND user = ?2",
            c.id,
            OWNER_USER,
        );
        ratioOk = (s?.since_recheck ?? 0) >= recheckEvery(env);
    }
    const usable = cands.filter((u) => (ratioOk || u.special_at <= late) && recheckOfferable(env, u.accepted_at ?? 0, now, c.lease_seconds));
    if (usable.length === 0) {
        return null;
    }
    const runs = await db.all<CandidateRun>(
        `SELECT unit_id, host, backend, state FROM runs
         WHERE unit_id IN (SELECT value FROM json_each(?1)) AND state IN ('leased', 'submitted')`,
        JSON.stringify(usable.map((u) => u.id)),
    );
    let ownHosts: Array<{ host: string; backend: string; recheck_cpu: number; bits: number }> | null = null;
    for (const u of usable) {
        const mine = runs.filter((r) => r.unit_id === u.id);
        if (mine.some((r) => r.state === "leased")) {
            continue; // a lease of it is out
        }
        const prior = mine.filter((r) => r.state === "submitted");
        const backend = chooseBackend(prior, o);
        if (u.special_at > late) {
            const kind = runKind(prior, o.host, backend);
            ownHosts ??= await db.all(
                `SELECT host, backend, recheck_cpu, bits FROM hosts
                 WHERE campaign_id = ?1 AND user = ?2 AND last_seen > ?3 LIMIT 32`,
                c.id,
                OWNER_USER,
                now - ACTIVE_SECONDS,
            );
            const better = ownHosts.some(
                (h) =>
                    h.host !== o.host &&
                    h.bits >= u.bits &&
                    RANK[runKind(prior, h.host, chooseBackend(prior, { backend: h.backend, recheckCpu: h.recheck_cpu === 1 }))] < RANK[kind],
            );
            if (better) {
                continue;
            }
        }
        const runId = await leaseRecheck(db, env, c, u, o.host, backend, now, false);
        if (runId !== null) {
            await db.run("UPDATE user_stats SET since_recheck = 0 WHERE campaign_id = ?1 AND user = ?2", c.id, OWNER_USER);
            return { unit: u, backend, runId };
        }
    }
    return null;
}

/**
 * Leases (or with `renewOnly`, renews a released or expired re-check run of)
 * the re-check of unit `u` to `host`, while it is pending, no lease of the
 * unit is out and the lease ends before the embargo. The conditions are
 * checked again in the statement, so concurrent fetches cannot both take it.
 * Returns the id of the leased run (the lease id), or null.
 */
export async function leaseRecheck(
    db: Db,
    env: Env,
    c: Campaign,
    u: { id: number },
    host: string,
    backend: string,
    now: number,
    renewOnly: boolean,
): Promise<number | null> {
    const cond = `u.id = ?1 AND u.campaign_id = ?6 AND u.state = 'accepted' AND u.recheck = 'pending' AND ?5 <= u.accepted_at + ?8
        AND NOT EXISTS (SELECT 1 FROM runs x WHERE x.unit_id = u.id AND x.state = 'leased')`;
    const args = [u.id, OWNER_USER, host, now, now + c.lease_seconds, c.id, backend, embargoSeconds(env)];
    const renewed = await db.all<{ id: number }>(
        `UPDATE runs SET state = 'leased', host = ?3, leased_at = ?4, expires_at = ?5
         WHERE id = (SELECT r.id FROM runs r JOIN units u ON u.id = r.unit_id
                     WHERE r.unit_id = ?1 AND r.user = ?2 AND r.role = 'recheck' AND r.state IN ('released', 'expired') AND ${cond}
                     ORDER BY r.id DESC LIMIT 1)
         RETURNING id`,
        ...args,
    );
    if (renewed.length === 1 || renewOnly) {
        return renewed[0]?.id ?? null;
    }
    const leased = await db.all<{ id: number }>(
        `INSERT INTO runs (unit_id, user, host, role, state, backend, leased_at, expires_at)
         SELECT u.id, ?2, ?3, 'recheck', 'leased', ?7, ?4, ?5 FROM units u WHERE ${cond}
         RETURNING id`,
        ...args,
    );
    return leased[0]?.id ?? null;
}
