// POST /api/submit takes a result on a live lease and derives its unit's state
// (units.ts derive): acceptance, the random re-check draw, a re-check that
// matches or disagrees (a conflict: the unit leaves the frontier and an alert
// is raised), records, the agreed document, the frontier and the ledger entry.
// The body names the lease (lease_id, from /api/fetch); a result on any other
// lease, or on one that is not live, never counts (submit/resend.ts). The
// write is one batch, guarded on the unit's rev, result count and block, on
// the run's unit and holder and on the ledger's head.

import { JsonObject, JsonValue, parseJson } from "./json";
import { Db, Env, HttpError, checkHost, json, nowSeconds } from "./http";
import { LedgerFields, pushLedger } from "./ledger";
import { ResultError } from "./result";
import { intField, isGuardFailure, leaseIdField } from "./units";
import { acceptanceWrites, alertWrites, campaignWrites } from "./submit/accept";
import { Attempt, Batch, Decision, LeaseRead, Submission, TestHooks } from "./submit/context";
import { decide } from "./submit/decide";
import { guardedWrites } from "./submit/guard";
import { readLease } from "./submit/read";

export type { TestHooks } from "./submit/context";

/**
 * Takes the result in body `b`. Each attempt reads the lease and the unit
 * (submit/read.ts), decides (submit/decide.ts) and writes one batch
 * (submit/guard.ts, accept.ts). A batch that loses a guard makes the next
 * attempt read again; after 4 attempts the answer is 503.
 */
export async function submit(db: Db, env: Env, b: JsonObject, hooks: TestHooks = {}): Promise<Response> {
    const testRace = hooks.race ?? 0;
    const host = checkHost(b["host"]);
    const unitId = intField(b, "unit_id");
    const leaseId = leaseIdField(b);
    let doc: JsonValue | undefined = b["result_json"];
    if (typeof doc === "string") {
        try {
            doc = parseJson(doc);
        } catch {
            throw new ResultError("result_json is not valid JSON");
        }
    }
    if (doc === undefined) {
        throw new HttpError(400, "result_json is missing");
    }
    const s: Submission = { db, env, hooks, testRace, host, unitId, leaseId, doc };
    for (let attempt = 0; attempt < 4; attempt++) {
        const a: Attempt = { attempt, now: nowSeconds() };
        const read = await readLease(s, a);
        if (read instanceof Response) {
            return read;
        }
        const dec = decide(s, a, read);
        const stmts = guardedWrites(s, a, read, dec);
        const ledger: LedgerFields[] = [];
        const batch: Batch = { stmts, ledger };
        acceptanceWrites(s, a, read, dec, batch);
        alertWrites(s, a, read, dec, batch);
        campaignWrites(s, a, read, dec, batch);
        await pushLedger(db, stmts, ledger, testRace === 3 && attempt === 0);
        try {
            await db.batch(stmts);
        } catch (e) {
            if (isGuardFailure(e)) {
                continue; // another submit for this unit or an owner decision won, or the lease ran out; re-read and decide again
            }
            throw e;
        }
        return answer(read, dec);
    }
    throw new HttpError(503, "the unit is busy; retry", { retry_after: 10 });
}

/** The answer to a submit whose batch was written: the unit's state and the verdict, which the worker prints. */
function answer(l: LeaseRead, dec: Decision): Response {
    if (l.row.role === "recheck") {
        // A re-check gets the answer of an accepting regular result, so it cannot be told apart.
        return json(200, { state: "accepted", verdict: "accepted" });
    }
    return json(200, { state: dec.d.state, verdict: dec.nowAccepted ? "accepted" : "pending" });
}
