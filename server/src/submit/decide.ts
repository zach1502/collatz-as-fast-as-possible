// Part of submit.ts: the decision of an attempt. The run as it is stored, the
// re-check it passes or fails, the state the unit's runs give it (units.ts),
// the re-check draw of a unit that this result accepts, since when the unit
// waits, and the digest and embargo start the unit keeps.

import { randomFraction } from "../http";
import { recheckOfferable, runKind } from "../recheck";
import { SubmittedRun, derive } from "../units";
import { Attempt, Decision, LeaseRead, Submission } from "./context";

/**
 * Decides what the result of attempt `a` does to the unit, from the reads `l`.
 */
export function decide(s: Submission, a: Attempt, l: LeaseRead): Decision {
    const { env, host } = s;
    const { now } = a;
    const { row, c, checked, others } = l;
    const kind = row.role === "regular" ? null : runKind(others, host, checked.backend);
    const mineRun: SubmittedRun = {
        id: row.run_id,
        host,
        role: row.role,
        backend: checked.backend,
        kind,
        digest: checked.digest,
        result_json: checked.canonical,
    };
    const runs = [...others, mineRun];
    let recheck = row.recheck;
    let recheckKind = row.recheck_kind;
    const failedNow = row.role === "recheck" && checked.digest !== row.udigest;
    if (row.role === "recheck") {
        recheck = failedNow ? "failed" : "passed";
        recheckKind = kind;
    }
    const d = derive(runs);
    const wasAccepted = row.ustate === "accepted";
    const nowAccepted = d.state === "accepted";
    if (nowAccepted && !wasAccepted && recheck === null) {
        // A unit that this result accepts is queued for a re-check at the
        // campaign's rate, if a re-check lease can end before its embargo does.
        if (randomFraction() < c.recheck_rate && recheckOfferable(env, now, now, c.lease_seconds)) {
            recheck = "pending";
        }
    }
    // A unit waits for its re-check from the moment it starts to; a conflict
    // waits for the owner's decision from its last result.
    const waits = (nowAccepted && recheck === "pending") || d.state === "conflict";
    const fromNow = d.state === "conflict" || (waits && (row.special_at === null || row.ustate !== d.state));
    const specialAt = fromNow ? now : waits ? row.special_at : null;
    const drawAt = fromNow ? now : waits ? row.draw_at : null;
    // A conflict keeps the digest it was accepted with, for the alert's outcome.
    const digest = nowAccepted ? d.digest : row.udigest;
    // A unit that this result accepts starts its embargo at `now`.
    const acceptedAt = nowAccepted ? (wasAccepted ? row.accepted_at : now) : null;
    return { kind, mineRun, runs, recheck, recheckKind, failedNow, d, wasAccepted, nowAccepted, specialAt, drawAt, digest, acceptedAt };
}
