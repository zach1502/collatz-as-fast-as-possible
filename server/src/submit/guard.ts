// Part of submit.ts: the guarded writes that open the batch of a submit. The
// unit's update matches only while its result count, rev and block are the
// ones the attempt read, and the run's update only while the lease is live and
// held by the sending machine. GUARD after each update fails the whole batch
// otherwise (units.ts), and the submit retries.

import { OWNER_USER } from "../http";
import { GUARD } from "../units";
import { Attempt, Decision, LeaseRead, Submission } from "./context";

/**
 * The first statements of the batch of attempt `a`: the guarded updates of
 * the unit and of the run, each followed by GUARD. TestHooks.race 1 and 2
 * make the unit or the run update of the first attempt match no row.
 */
export function guardedWrites(s: Submission, a: Attempt, l: LeaseRead, dec: Decision): D1PreparedStatement[] {
    const { db, testRace, host, unitId } = s;
    const { attempt, now } = a;
    const { row, c, block, checked } = l;
    const { kind, recheck, recheckKind, d, nowAccepted, specialAt, drawAt, digest, acceptedAt } = dec;
    return [
        // The WHERE checks rev (an owner decision after the attempt's reads
        // changes the unit) and the block (the result was checked against it).
        db.prep(
            `UPDATE units SET results = results + 1, wanted = ?1, state = ?2, disputed = MAX(disputed, ?3), digest = ?4,
                     checks = ?5, accepted_at = ?6, recheck = ?7, recheck_kind = ?8, special_at = ?9, draw_at = ?10, rev = rev + 1
                 WHERE id = ?11 AND results = ?12 AND rev = ?13 AND campaign_id = ?14 AND pos = ?15 AND bits = ?16`,
            d.wanted,
            d.state,
            d.disputed,
            digest,
            d.checks,
            acceptedAt,
            recheck,
            recheckKind,
            specialAt,
            drawAt,
            unitId,
            testRace === 1 && attempt === 0 ? row.results - 1 : row.results,
            row.rev,
            c.id,
            block.pos,
            block.bits,
        ),
        db.prep(GUARD),
        // An accepted unit keeps one copy of the agreed document (unit_results);
        // a run stores its own only while it disagrees or the unit is not accepted.
        // The lease must still be live and this machine's.
        db.prep(
            `UPDATE runs SET state = 'submitted', host = ?1, submitted_at = ?2, digest = ?3, result_json = ?4, backend = ?5, kind = ?6
                 WHERE id = ?7 AND state = 'leased' AND expires_at > ?2 AND ?8 = 0 AND unit_id = ?9 AND user = ?10 AND host = ?1`,
            host,
            now,
            checked.digest,
            nowAccepted && digest === checked.digest ? null : checked.canonical,
            checked.backend,
            kind,
            row.run_id,
            testRace === 2 && attempt === 0 ? 1 : 0,
            unitId,
            OWNER_USER,
        ),
        db.prep(GUARD),
    ];
}
