// Part of submit.ts: the batch statements and ledger entries of acceptance,
// the re-check pacing count and the alert of a disagreeing re-check. A unit
// that leaves the accepted state (a re-check disagreed) gets a 'withdrawn'
// entry, an alert and loses its agreed document, records and block stats. A
// unit that this result accepts gets a 'block' entry, the agreed document, its
// records and its block stats. The campaign row then moves the frontier, and
// a re-check that agrees gets a 'recheck' entry.

import { OWNER_USER } from "../http";
import { resultFields, withdrawnFields } from "../ledger";
import { countRegularResult } from "../recheck";
import { fieldDiff } from "../result";
import { blockStatsStatements, removeBlockStats } from "../stats";
import { AlertRun, frontierSql, recordsInsert } from "../units";
import { Attempt, Batch, Decision, LeaseRead, Submission } from "./context";

/** Adds to `b` the statements and ledger entries of a unit that this result withdraws or accepts. */
export function acceptanceWrites(s: Submission, a: Attempt, l: LeaseRead, dec: Decision, b: Batch): void {
    const { db, host, unitId } = s;
    const { now } = a;
    const { row, c, block, checked } = l;
    const { d, wasAccepted, nowAccepted, digest } = dec;
    const { stmts, ledger } = b;
    if (wasAccepted && !nowAccepted) {
        // The block stops counting with the digest it was accepted with. The
        // agreed document goes back to the runs that agreed on it, as on any
        // unit that is not accepted. Records and block stats are kept for
        // accepted units only.
        ledger.push(withdrawnFields(c, block, now, row.udigest ?? ""));
        stmts.push(
            db.prep(
                `UPDATE runs SET result_json = (SELECT result_json FROM unit_results WHERE unit_id = ?1)
                     WHERE unit_id = ?1 AND state = 'submitted' AND digest = ?2 AND result_json IS NULL`,
                unitId,
                row.udigest,
            ),
            db.prep("DELETE FROM unit_results WHERE unit_id = ?1", unitId),
            db.prep("DELETE FROM records WHERE campaign_id = ?1 AND unit_id = ?2", c.id, unitId),
            removeBlockStats(db, unitId),
        );
    }
    if (nowAccepted && !wasAccepted) {
        // The unit's only result accepts it.
        ledger.push(resultFields("block", c, block, now, digest ?? "", checked.canonical, { host, backend: checked.backend }, d.checks));
        stmts.push(db.prep("INSERT INTO unit_results (unit_id, result_json) VALUES (?1, ?2)", unitId, checked.canonical));
        stmts.push(...recordsInsert(db, c.id, unitId, checked.records, now));
        stmts.push(...blockStatsStatements(db, c, { id: unitId, pos: block.pos, bits: block.bits, len: row.len }, now, host, checked.figures));
    }
}

/**
 * Adds to `b` the update of the campaign's frontier and the ledger entry of a
 * re-check that agrees.
 */
export function campaignWrites(s: Submission, a: Attempt, l: LeaseRead, dec: Decision, b: Batch): void {
    const { db, host } = s;
    const { now } = a;
    const { row, c, block, checked } = l;
    const { failedNow, d, wasAccepted, nowAccepted } = dec;
    const { stmts, ledger } = b;
    stmts.push(
        db.prep(
            `UPDATE campaigns SET frontier_pos = ${frontierSql("?1", "?2", "?3", "?4")} WHERE id = ?1`,
            c.id,
            block.pos,
            nowAccepted && !wasAccepted ? 1 : 0,
            wasAccepted && !nowAccepted ? 1 : 0,
        ),
    );
    if (row.role === "recheck" && !failedNow) {
        ledger.push(resultFields("recheck", c, block, now, checked.digest, checked.canonical, { host, backend: checked.backend }, d.checks));
    }
}

/** Adds to `b` the pacing count of a regular result and the alert of a re-check that disagrees. */
export function alertWrites(s: Submission, a: Attempt, l: LeaseRead, dec: Decision, b: Batch): void {
    const { db, unitId } = s;
    const { now } = a;
    const { row, c, checked, agreedOld } = l;
    const { kind, runs, failedNow } = dec;
    const { stmts } = b;
    if (row.role === "regular") {
        stmts.push(countRegularResult(db, c.id, now));
    }
    if (failedNow) {
        const involved: AlertRun[] = runs.map((r) => ({ user: OWNER_USER, host: r.host, role: r.role, kind: r.kind, backend: r.backend, digest: r.digest }));
        stmts.push(
            db.prep(
                "INSERT INTO alerts (campaign_id, unit_id, kind, fields, runs, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                c.id,
                unitId,
                kind,
                JSON.stringify(fieldDiff([agreedOld ?? "", checked.canonical])),
                JSON.stringify(involved),
                now,
            ),
        );
    }
}
