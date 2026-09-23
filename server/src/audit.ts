// POST /api/audit: the outcome of a machine's random certify audit of a unit
// it ran, which re-derives in GMP the sieve decisions for residues drawn from
// the run's result digest. The audit must be of a result the owner's machines
// submitted from the same host that its unit, which must be accepted, was
// accepted with. Each one is kept in `audits` and counted per campaign for the
// page ("Audits: N passed, M failed."). One with failures raises an alert, as
// a disagreeing re-check does. The audit is the machine's own report, so the
// unit is not changed. The owner resolves the alert, or withdraws the block
// with `admin.mjs decide --withdraw` (admin_units.ts), which takes it out of
// the frontier, reopens it for a new result and deletes its audits.

import { JsonNumber, JsonObject } from "./json";
import { Db, HttpError, OWNER_USER, checkHost, json, nowSeconds } from "./http";
import { AlertRun, GUARD, intField, isGuardFailure } from "./units";

/** The most residues one audit draws (certify::kMaxAuditSamples). */
const MAX_AUDIT_SAMPLES = 10000000;
const DIGEST = /^[0-9a-f]{64}$/;

interface AuditFields {
    host: string;
    unitId: number;
    seed: string;
    samples: number;
    failures: number;
    seconds: number;
}

/** An alert's run, with the audit's figures (admin.mjs prints them). */
interface AuditAlertRun extends AlertRun {
    samples: number;
    failures: number;
    seconds: number;
}

function auditFields(b: JsonObject): AuditFields {
    const host = checkHost(b["host"]);
    const unitId = intField(b, "unit_id");
    const seed = b["seed"];
    if (typeof seed !== "string" || !DIGEST.test(seed)) {
        throw new HttpError(400, "seed must be the audited result's result_digest (64 lowercase hex digits)");
    }
    const samples = intField(b, "samples");
    if (samples < 1 || samples > MAX_AUDIT_SAMPLES) {
        throw new HttpError(400, `samples must be an integer from 1 to ${MAX_AUDIT_SAMPLES}`);
    }
    const failures = intField(b, "failures");
    const v = b["seconds"];
    const seconds = v instanceof JsonNumber ? Number(v.raw) : NaN;
    if (!(Number.isFinite(seconds) && seconds >= 0 && seconds <= 1e9)) {
        throw new HttpError(400, "seconds must be a number from 0 to 1e9");
    }
    return { host, unitId, seed, samples, failures, seconds };
}

/**
 * POST /api/audit {token, host, unit_id, seed, samples, failures, seconds}:
 * stores the audit of the submitted run of the unit on `host` whose digest is
 * `seed`: a run of an accepted unit with the digest the unit was accepted
 * with (409 otherwise). The same audit sent again answers duplicate: true;
 * other figures for it answer 409.
 */
export async function audit(db: Db, b: JsonObject): Promise<Response> {
    const a = auditFields(b);
    const run = await db.first<{ campaign_id: number; backend: string; state: string; accepted: number }>(
        `SELECT u.campaign_id, r.backend, u.state, u.digest IS r.digest AS accepted FROM runs r JOIN units u ON u.id = r.unit_id
         WHERE r.unit_id = ?1 AND r.user = ?2 AND r.host = ?3 AND r.state = 'submitted' AND r.digest = ?4 LIMIT 1`,
        a.unitId,
        OWNER_USER,
        a.host,
        a.seed,
    );
    if (run === null) {
        throw new HttpError(409, `you submitted no result of unit ${a.unitId} from this host with this result_digest`);
    }
    if (run.state !== "accepted") {
        throw new HttpError(409, `unit ${a.unitId} is ${run.state}; only the result of an accepted unit is audited`);
    }
    if (run.accepted !== 1) {
        throw new HttpError(409, `unit ${a.unitId} was accepted with another result than yours, so its audit is not taken`);
    }
    const now = nowSeconds();
    const failed = a.failures > 0 ? 1 : 0;
    const stmts: D1PreparedStatement[] = [
        db.prep(
            `INSERT INTO audits (campaign_id, unit_id, user, host, seed, samples, failures, seconds, created_at)
             SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9
             WHERE NOT EXISTS (SELECT 1 FROM audits WHERE unit_id = ?2 AND user = ?3 AND host = ?4 AND seed = ?5)`,
            run.campaign_id,
            a.unitId,
            OWNER_USER,
            a.host,
            a.seed,
            a.samples,
            a.failures,
            a.seconds,
            now,
        ),
        db.prep(GUARD),
        db.prep(
            "UPDATE campaigns SET audits_passed = audits_passed + ?1, audits_failed = audits_failed + ?2 WHERE id = ?3",
            1 - failed,
            failed,
            run.campaign_id,
        ),
    ];
    if (failed === 1) {
        const runs: AuditAlertRun[] = [
            {
                user: OWNER_USER,
                host: a.host,
                role: "audit",
                kind: null,
                backend: run.backend,
                digest: a.seed,
                samples: a.samples,
                failures: a.failures,
                seconds: a.seconds,
            },
        ];
        stmts.push(
            db.prep(
                "INSERT INTO alerts (campaign_id, unit_id, kind, fields, runs, created_at) VALUES (?1, ?2, 'audit', '[]', ?3, ?4)",
                run.campaign_id,
                a.unitId,
                JSON.stringify(runs),
                now,
            ),
        );
    }
    try {
        await db.batch(stmts);
    } catch (e) {
        if (!isGuardFailure(e)) {
            throw e;
        }
        // This run already reported an audit of this seed.
        const old = await db.first<{ samples: number; failures: number }>(
            "SELECT samples, failures FROM audits WHERE unit_id = ?1 AND user = ?2 AND host = ?3 AND seed = ?4",
            a.unitId,
            OWNER_USER,
            a.host,
            a.seed,
        );
        if (old !== null && old.samples === a.samples && old.failures === a.failures) {
            return json(200, { ok: true, duplicate: true, unit_id: a.unitId });
        }
        throw new HttpError(409, `this host already reported an audit of unit ${a.unitId} with other figures`);
    }
    return json(200, { ok: true, unit_id: a.unitId });
}
