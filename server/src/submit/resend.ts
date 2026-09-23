// Part of submit.ts: the answer to a result that arrives on no live lease of
// the machine that sends it. The lease was handed back, ran out, went to
// another machine, already took a result, or no longer exists because the
// unit was started over (admin_units.ts). Such a result never counts; it is
// compared with the unit's submitted runs instead.

import { HttpError, OWNER_USER, json } from "../http";
import { Campaign, fieldDiff } from "../result";
import { AlertRun, SUBMITTED_RUN_COLS, SubmittedRun, lapsedError } from "../units";
import { Attempt, LeaseRead, MineRow, Submission } from "./context";

/** The run of a kept result in a 'resend' alert: the lease it came on and its document. */
interface ResendAlertRun extends AlertRun {
    lease_id: number;
    result_json: string;
}

/**
 * Answers the result `checked` for a block of `c` that arrived on lease `row`
 * (null when it no longer exists), which is not live for the sending
 * machine. A digest that one of the unit's submitted runs has answers
 * duplicate: the result is already there (a copy sent again after a lost
 * answer, or the same result from the machine that took the lease over). A
 * digest that differs from all of them is kept in an alert of kind 'resend'
 * (the runs, the fields that differ and the document), once per digest: a
 * later copy finds its digest there and raises no second alert. Without a
 * submitted run the lease lapsed: 409 with lease_state when the lease still
 * exists, so that the machine can ask for it again (prefer_unit), and 409
 * without it when it does not.
 */
export async function notLive(s: Submission, a: Attempt, c: Campaign, checked: LeaseRead["checked"], row: MineRow | null): Promise<Response> {
    const { db, host, unitId, leaseId } = s;
    const submitted = await db.all<SubmittedRun>(
        `SELECT ${SUBMITTED_RUN_COLS} FROM runs WHERE unit_id = ?1 AND state = 'submitted' ORDER BY submitted_at, id`,
        unitId,
    );
    if (submitted.some((r) => r.digest === checked.digest)) {
        return json(200, { duplicate: true });
    }
    if (submitted.length > 0) {
        const id = await keepResend(s, a, c, checked, submitted);
        const why =
            row !== null && row.rstate === "submitted" && row.rhost === host
                ? `you already submitted a different result for unit ${unitId} on lease ${leaseId}`
                : `this result of unit ${unitId} came on lease ${leaseId}, which takes no result now, and differs from the unit's`;
        throw new HttpError(409, `${why}; it is kept for the owner (alert ${id}) and does not count`, { rejected: true, alert_id: id });
    }
    if (row === null) {
        throw new HttpError(409, `lease ${leaseId} of unit ${unitId} does not exist (the unit was started over); the result does not count`);
    }
    if (row.rhost !== host) {
        throw new HttpError(409, `your lease of unit ${unitId} lapsed and went to another machine; fetch the unit again (prefer_unit) to submit`, {
            lease_state: row.rstate === "released" ? "released" : "expired",
        });
    }
    throw lapsedError(unitId, row.rstate === "released" ? "released" : "expired");
}

/**
 * Keeps the result `checked` of unit `s.unitId`, which differs from every
 * one of its `submitted` runs, in a 'resend' alert, unless an alert of the
 * unit already keeps this digest. Returns the id of the alert that keeps it.
 */
async function keepResend(s: Submission, a: Attempt, c: Campaign, checked: LeaseRead["checked"], submitted: SubmittedRun[]): Promise<number> {
    const { db, host, unitId, leaseId } = s;
    const agreed = await db.first<{ result_json: string }>("SELECT result_json FROM unit_results WHERE unit_id = ?1", unitId);
    const reference = agreed?.result_json ?? submitted.find((r) => r.result_json !== null)?.result_json ?? "";
    const runs: Array<AlertRun | ResendAlertRun> = submitted.map((r) => ({ user: OWNER_USER, host: r.host, role: r.role, kind: r.kind, backend: r.backend, digest: r.digest }));
    runs.push({
        user: OWNER_USER,
        host,
        role: "resend",
        kind: null,
        backend: checked.backend,
        digest: checked.digest,
        lease_id: leaseId,
        result_json: checked.canonical,
    });
    // The digest is looked up among the kept runs of the unit's 'resend' alerts
    // (alerts_unit index), in the insert itself, so two copies at once keep one.
    const kept = (unit: string, digest: string): string =>
        `SELECT a.id FROM alerts a, json_each(a.runs) j
         WHERE a.unit_id = ${unit} AND a.kind = 'resend' AND json_extract(j.value, '$.role') = 'resend'
           AND json_extract(j.value, '$.digest') = ${digest}`;
    const inserted = await db.all<{ id: number }>(
        `INSERT INTO alerts (campaign_id, unit_id, kind, fields, runs, created_at)
         SELECT ?1, ?2, 'resend', ?3, ?4, ?5 WHERE NOT EXISTS (${kept("?2", "?6")})
         RETURNING id`,
        c.id,
        unitId,
        JSON.stringify(fieldDiff([reference, checked.canonical])),
        JSON.stringify(runs),
        a.now,
        checked.digest,
    );
    const id = inserted[0]?.id ?? (await db.first<{ id: number }>(`${kept("?1", "?2")} LIMIT 1`, unitId, checked.digest))?.id;
    return id ?? 0;
}
