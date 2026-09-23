// Part of submit.ts: the reads of an attempt. The lease the result is sent on
// with the unit's state, the checks of the lease and of the result, the
// unit's other submitted runs and, for a re-check, the agreed document. A
// result on a lease that is not live goes to resend.ts.

import { HttpError, OWNER_USER, embargoSeconds } from "../http";
import { checkResult } from "../result";
import { SUBMITTED_RUN_COLS, SubmittedRun, campaignById, lapsedError, requireRunnable } from "../units";
import { Attempt, LeaseRead, MineRow, Submission } from "./context";
import { notLive } from "./resend";

/**
 * Reads lease `s.leaseId` of the unit and the unit's state for attempt `a`
 * and checks the result against the unit's block. The lease takes the result
 * only while it is leased, has not run out and is held by the machine that
 * sends it; otherwise resend.ts gives the answer (a duplicate, a differing
 * result kept for the owner, or a lapsed lease). Throws when there is no such
 * unit, a re-check lease lapsed or the unit takes no more results.
 */
export async function readLease(s: Submission, a: Attempt): Promise<LeaseRead | Response> {
    const { db, env, hooks, host, unitId, leaseId, doc } = s;
    const { attempt, now } = a;
    const row = await db.first<MineRow>(
        `SELECT r.id AS run_id, r.state AS rstate, r.role, r.host AS rhost, r.expires_at, r.digest AS rdigest, u.campaign_id, u.pos,
                u.bits, u.len, u.state AS ustate, u.results, u.digest AS udigest, u.accepted_at, u.recheck, u.recheck_kind,
                u.special_at, u.draw_at, u.rev
         FROM runs r JOIN units u ON u.id = r.unit_id WHERE r.id = ?1 AND r.unit_id = ?2 AND r.user = ?3`,
        leaseId,
        unitId,
        OWNER_USER,
    );
    const unit = row ?? (await db.first<{ campaign_id: number; pos: number; bits: number }>("SELECT campaign_id, pos, bits FROM units WHERE id = ?1", unitId));
    if (unit === null) {
        throw new HttpError(409, `you hold no lease for unit ${unitId}`);
    }
    const c = requireRunnable(await campaignById(db, unit.campaign_id));
    const block = { pos: unit.pos, bits: unit.bits };
    const checked = await checkResult(doc, c, block);
    if (row === null || row.rstate !== "leased" || row.expires_at <= now || row.rhost !== host) {
        return notLive(s, a, c, checked, row);
    }
    if (row.role === "recheck") {
        // A re-check ends with the embargo. It answers like any lapsed lease.
        if (row.ustate !== "accepted" || row.recheck !== "pending" || now >= (row.accepted_at ?? 0) + embargoSeconds(env)) {
            throw lapsedError(unitId, "expired");
        }
    } else if (row.ustate !== "open") {
        throw new HttpError(409, `unit ${unitId} is ${row.ustate} and takes no more results`);
    }
    const others = await db.all<SubmittedRun>(
        `SELECT ${SUBMITTED_RUN_COLS} FROM runs WHERE unit_id = ?1 AND state = 'submitted' ORDER BY submitted_at, id`,
        unitId,
    );
    const agreedOld =
        row.role === "recheck"
            ? ((await db.first<{ result_json: string }>("SELECT result_json FROM unit_results WHERE unit_id = ?1", unitId))
                  ?.result_json ?? null)
            : null;
    if (attempt === 0 && hooks.afterRead !== undefined) {
        await hooks.afterRead();
    }
    return { row, c, block, checked, others, agreedOld };
}
