// Owner endpoints that act on units (admin.ts describes the admin API):
// decide, which accepts a unit in conflict with one of its results, starts it
// over, or withdraws an accepted unit.

import { JsonObject } from "./json";
import { Db, HttpError, json, nowSeconds } from "./http";
import { pushLedger, resultFields, withdrawnFields } from "./ledger";
import { storedFigures, storedRecords } from "./result";
import { blockStatsStatements, removeBlockStats } from "./stats";
import { GUARD, SUBMITTED_RUN_COLS, SubmittedRun, UnitRow, campaignById, frontierSql, intField, isGuardFailure, recordsInsert } from "./units";

const NOTE = /^[ -~]{1,200}$/;

/** The body's `note`: 1-200 characters of printable ASCII. */
export function noteOf(b: JsonObject): string {
    const note = b["note"];
    if (typeof note !== "string" || !NOTE.test(note)) {
        throw new HttpError(400, "note must be 1-200 characters of printable ASCII");
    }
    return note;
}

/** Runs the batch `stmts`; false when a guard fails because a concurrent update came first. */
async function guarded(db: Db, stmts: D1PreparedStatement[]): Promise<boolean> {
    try {
        await db.batch(stmts);
    } catch (e) {
        if (isGuardFailure(e)) {
            return false;
        }
        throw e;
    }
    return true;
}

/**
 * Accepts unit `unitId`, in conflict, with `digest`, which one of its results
 * must have, with a new embargo and no further re-check. The first run with
 * that digest gives the agreed document, the records and block stats and the
 * 'block' ledger entry; the unit's alerts are settled ('owner') and resolved
 * with `note`, and the frontier moves. False when a concurrent update won the
 * guard.
 */
async function accept(db: Db, unitId: number, digest: string, note: string): Promise<boolean> {
    const u = await db.first<UnitRow & { agreed_json: string | null; has_agreed: number }>(
        `SELECT u.*, d.result_json AS agreed_json, d.unit_id IS NOT NULL AS has_agreed
         FROM units u LEFT JOIN unit_results d ON d.unit_id = u.id WHERE u.id = ?1`,
        unitId,
    );
    if (u === null) {
        throw new HttpError(404, `no unit ${unitId}`);
    }
    if (u.state !== "conflict") {
        throw new HttpError(409, `unit ${unitId} is ${u.state}; only a unit in conflict waits for a decision`);
    }
    const c = await campaignById(db, u.campaign_id);
    const runs = await db.all<SubmittedRun>(
        `SELECT ${SUBMITTED_RUN_COLS} FROM runs WHERE unit_id = ?1 AND state = 'submitted' ORDER BY submitted_at, id`,
        unitId,
    );
    const ran = runs.find((r) => r.digest === digest);
    if (ran === undefined) {
        throw new HttpError(400, `no result of unit ${unitId} has digest ${digest}`);
    }
    const now = nowSeconds();
    const stmts: D1PreparedStatement[] = [
        // The rev guard fails if a submit or another decision changed the unit since the reads above.
        db.prep(
            `UPDATE units SET state = 'accepted', digest = ?1, wanted = ?2, checks = 1, accepted_at = ?3, special_at = NULL,
                 draw_at = NULL, rev = rev + 1
             WHERE id = ?4 AND rev = ?5 AND state = 'conflict'`,
            digest,
            runs.length,
            now,
            unitId,
            u.rev,
        ),
        db.prep(GUARD),
        // The unit takes no run once it is decided: a live lease of it is handed back.
        db.prep("UPDATE runs SET state = 'released' WHERE unit_id = ?1 AND state = 'leased'", unitId),
    ];
    // The agreed document: kept when the unit still holds one of this digest;
    // otherwise any other goes back to the runs that agreed on it, and one
    // copy of this digest's comes from its runs.
    const keepDoc = u.has_agreed === 1 && u.digest === digest;
    if (u.has_agreed === 1 && !keepDoc) {
        stmts.push(
            db.prep(
                `UPDATE runs SET result_json = (SELECT result_json FROM unit_results WHERE unit_id = ?1)
                 WHERE unit_id = ?1 AND state = 'submitted' AND digest = ?2 AND result_json IS NULL`,
                unitId,
                u.digest,
            ),
            db.prep("DELETE FROM unit_results WHERE unit_id = ?1", unitId),
        );
    }
    let agreedDoc = keepDoc ? u.agreed_json : null;
    if (!keepDoc) {
        agreedDoc = ran.result_json ?? runs.find((r) => r.digest === digest && r.result_json !== null)?.result_json ?? null;
        stmts.push(db.prep("DELETE FROM records WHERE campaign_id = ?1 AND unit_id = ?2", c.id, unitId));
        if (agreedDoc !== null) {
            stmts.push(
                db.prep("INSERT INTO unit_results (unit_id, result_json) VALUES (?1, ?2)", unitId, agreedDoc),
                db.prep(
                    "UPDATE runs SET result_json = NULL WHERE unit_id = ?1 AND state = 'submitted' AND digest = ?2 AND result_json IS NOT NULL",
                    unitId,
                    digest,
                ),
            );
            stmts.push(...recordsInsert(db, c.id, unitId, storedRecords(agreedDoc), now));
            stmts.push(...blockStatsStatements(db, c, u, now, ran.host, storedFigures(agreedDoc)));
        }
    }
    stmts.push(
        db.prep("UPDATE alerts SET outcome = 'owner', settled_at = ?1 WHERE unit_id = ?2 AND outcome IS NULL", now, unitId),
        db.prep("UPDATE alerts SET resolved_at = ?1, note = ?2 WHERE unit_id = ?3 AND resolved_at IS NULL", now, note, unitId),
        db.prep(`UPDATE campaigns SET frontier_pos = ${frontierSql("?1", "?2", "1", "0")} WHERE id = ?1`, c.id, u.pos),
    );
    await pushLedger(db, stmts, [resultFields("block", c, u, now, digest, ran.result_json ?? agreedDoc, ran, 1)]);
    return guarded(db, stmts);
}

/**
 * Starts unit `unitId` over: its runs and audits are deleted (its alerts keep
 * the runs' hosts, backends and digests, and the audits' figures), and it
 * opens for one new result. Without `withdraw` the unit must be in conflict
 * (decide with requeue). With it the unit must be accepted (the owner's call
 * after a failed audit, decide with withdraw): it then leaves the frontier,
 * which moves back to it, with a 'withdrawn' ledger entry in the same batch,
 * and loses its agreed document, records and block statistics. A done
 * re-check stays recorded; a pending one is dropped. Its open alerts are
 * resolved with `note` (outcome 'requeued' unless already settled). False when
 * a concurrent update won a guard.
 */
async function startOver(db: Db, unitId: number, note: string, withdraw: boolean): Promise<boolean> {
    const u = await db.first<UnitRow>("SELECT * FROM units WHERE id = ?1", unitId);
    if (u === null) {
        throw new HttpError(404, `no unit ${unitId}`);
    }
    if (withdraw && u.state !== "accepted") {
        throw new HttpError(409, `unit ${unitId} is ${u.state}; only an accepted unit is withdrawn (requeue starts over a unit in conflict)`);
    }
    if (!withdraw && u.state !== "conflict") {
        throw new HttpError(
            409,
            `unit ${unitId} is ${u.state}; only a unit in conflict waits for a decision` +
                (u.state === "accepted" ? " (withdraw: true takes an accepted unit out of the frontier)" : ""),
        );
    }
    const c = await campaignById(db, u.campaign_id);
    const now = nowSeconds();
    const stmts: D1PreparedStatement[] = [
        // assigned is set here: deleting runs does not run the runs_assign_* triggers.
        db.prep(
            `UPDATE units SET state = 'open', results = 0, assigned = 0, wanted = ?1, checks = 0, digest = NULL, disputed = 0,
                 accepted_at = NULL, recheck = CASE WHEN recheck = 'pending' THEN NULL ELSE recheck END, special_at = NULL,
                 draw_at = NULL, rev = rev + 1
             WHERE id = ?2 AND rev = ?3 AND state = ?4`,
            c.quorum,
            unitId,
            u.rev,
            u.state,
        ),
        db.prep(GUARD),
        db.prep("DELETE FROM runs WHERE unit_id = ?1", unitId),
        db.prep("DELETE FROM audits WHERE unit_id = ?1", unitId),
        db.prep("DELETE FROM unit_results WHERE unit_id = ?1", unitId),
        db.prep("DELETE FROM records WHERE campaign_id = ?1 AND unit_id = ?2", c.id, unitId),
        db.prep(
            `UPDATE alerts SET outcome = COALESCE(outcome, 'requeued'), settled_at = COALESCE(settled_at, ?1), resolved_at = ?1, note = ?2
             WHERE unit_id = ?3 AND resolved_at IS NULL`,
            now,
            note,
            unitId,
        ),
        db.prep(`UPDATE campaigns SET frontier_pos = ${frontierSql("?1", "?2", "0", "?3")} WHERE id = ?1`, c.id, u.pos, withdraw ? 1 : 0),
    ];
    if (withdraw) {
        stmts.push(removeBlockStats(db, unitId));
        await pushLedger(db, stmts, [withdrawnFields(c, u, now, u.digest ?? "")]);
    }
    return guarded(db, stmts);
}

/**
 * POST /api/admin/decide {unit_id, digest | requeue: true | withdraw: true,
 * note}: the owner's decision on a unit. `digest` accepts a unit in conflict
 * with that digest, which one of its results must have, with a new embargo;
 * `requeue` starts such a unit over, and `withdraw` an accepted one, for
 * example after an audit found failures. The unit's open alerts are resolved
 * with `note`.
 */
export async function decide(db: Db, b: JsonObject): Promise<Response> {
    const unitId = intField(b, "unit_id");
    const note = noteOf(b);
    const digest = b["digest"];
    const again = b["requeue"];
    const withdraw = b["withdraw"];
    for (const [key, v] of [
        ["requeue", again],
        ["withdraw", withdraw],
    ] as const) {
        if (v !== undefined && v !== true) {
            throw new HttpError(400, `${key} must be true`);
        }
    }
    if ([digest, again, withdraw].filter((v) => v !== undefined).length !== 1) {
        throw new HttpError(
            400,
            "give one of digest (to accept that result), requeue: true (to run a unit in conflict again from scratch) " +
                "and withdraw: true (to take an accepted unit out of the frontier and run it again)",
        );
    }
    if (digest !== undefined && (typeof digest !== "string" || !/^[0-9a-f]{64}$/.test(digest))) {
        throw new HttpError(400, "digest must be 64 lowercase hex digits");
    }
    for (let attempt = 0; attempt < 4; attempt++) {
        const done = typeof digest === "string" ? await accept(db, unitId, digest, note) : await startOver(db, unitId, note, withdraw === true);
        if (done) {
            const u = await db.first<{ state: string; digest: string | null }>("SELECT state, digest FROM units WHERE id = ?1", unitId);
            return json(200, { ok: true, unit_id: unitId, state: u?.state ?? null, digest: u?.digest ?? null });
        }
    }
    throw new HttpError(503, `unit ${unitId} is busy; repeat the request`, { retry_after: 10 });
}
