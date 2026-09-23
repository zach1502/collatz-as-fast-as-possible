// npm test: conflicts and their alerts, the owner's decisions, and the
// withdrawal of an accepted block after a failed audit. The Worker and the
// helpers are in test/helpers-campaign.mjs.

import assert from "node:assert/strict";
import { test } from "node:test";

import { BARINA_L0 } from "../scripts/campaign.mjs";
import {
    CAMPAIGNS,
    DAY,
    activate,
    admin,
    adminScript,
    auditFor,
    campaignRow,
    digestOf,
    due,
    fetchFor,
    frontierOf,
    ledgerOf,
    machines,
    measure,
    newApi,
    nowS,
    pow,
    releaseFor,
    runOne,
    serveCampaigns,
    skip,
    srv,
    statsOf,
    submitFor,
    unitRow,
    wrongFor,
} from "./helpers-campaign.mjs";

serveCampaigns();

test("conflict: a disagreeing re-check takes the unit out of the frontier and raises an alert with the differing fields; it waits for the owner", { skip }, async () => {
    const id = CAMPAIGNS.conflict;
    const m = machines(2);
    const u0 = await runOne(m, id);
    const u1 = await runOne(m, id);
    assert.equal(await frontierOf(id), (BARINA_L0 + 2n * pow(32)).toString());
    await activate(id, m, 1);
    await due(id);
    const f = await fetchFor(m, id, {}, 1);
    assert.equal(f.body.lease.unit_id, u0.lease.unit_id, "the oldest waiting re-check first, on the other host");
    const s = await submitFor(m, u0.lease.unit_id, wrongFor(f.body.lease), 1);
    measure("submit (re-check disagrees: conflict, alert)", s);
    assert.deepEqual(s.body, u0.submit, "the answer does not tell the re-checker either");
    const row = await unitRow(u0.lease.unit_id);
    assert.deepEqual([row.state, row.recheck, row.recheck_kind, row.wanted, row.checks], ["conflict", "failed", "host", 3, 0]);
    assert.equal(row.digest, digestOf(u0.lease), "the accepted digest is kept for the outcome");
    assert.equal(await frontierOf(id), BARINA_L0.toString(), "the frontier rolls back to the conflict");
    const c = await campaignRow(id);
    assert.deepEqual([c.units_accepted, c.units_conflict, c.conflicts_total, c.rechecks_failed], [1, 1, 1, 1]);
    assert.equal((await statsOf(id)).alerts_open, 1);
    const al = await admin("GET", "/api/admin/alerts");
    assert.equal(al.status, 200, al.text);
    const mine = al.body.alerts.find((x) => x.unit_id === u0.lease.unit_id);
    assert.deepEqual(
        [mine.kind, mine.fields, mine.outcome, mine.state, mine.needs, mine.h, mine.k],
        ["host", ["blocks_total"], null, "conflict", "needs an owner decision", u0.lease.h, 32],
    );
    assert.deepEqual(
        mine.runs.map((x) => [x.user, x.host, x.role, x.kind]),
        [["owner", m.hosts[0], "regular", null], ["owner", m.hosts[1], "recheck", "host"]],
    );
    assert.notEqual(mine.runs[0].digest, mine.runs[1].digest);
    const listed = await adminScript(["alerts"]);
    assert.equal(listed.code, 0, listed.stderr);
    assert.match(listed.stdout, new RegExp(`unit ${u0.lease.unit_id} .*re-check by host; differs in blocks_total; not settled yet; needs an owner decision`));

    // No run settles a conflict: fetches pass it by, also after RECHECK_MAX_WAIT_SECONDS.
    await srv.query(`UPDATE units SET special_at = special_at - ${2 * DAY} WHERE id = ${u0.lease.unit_id}`);
    for (const h of [0, 1]) {
        const g = await fetchFor(m, id, {}, h);
        assert.equal(g.status, 200, g.text);
        assert.notEqual(g.body.lease?.unit_id, u0.lease.unit_id, `host ${h}`);
        if (g.body.lease) {
            await releaseFor(m, g.body.lease.unit_id, h);
        }
    }
    const r1 = await admin("POST", "/api/admin/alerts/resolve", { id: mine.id, note: "the second machine's memory, retested" });
    assert.deepEqual([r1.status, r1.body.resolved], [200, true], r1.text);
    assert.equal((await statsOf(id)).alerts_open, 0);
    assert.equal((await admin("POST", "/api/admin/alerts/resolve", { id: mine.id, note: "again" })).body.resolved, false);
    assert.equal((await admin("POST", "/api/admin/alerts/resolve", { id: mine.id })).status, 400, "a note is required");
    assert.equal((await unitRow(u0.lease.unit_id)).state, "conflict", "resolving the alert leaves the unit as it is");
    assert.equal((await unitRow(u1.lease.unit_id)).recheck, "pending", "the other unit's re-check still waits");
});

test("owner decision: a conflict is accepted with the owner's digest, or started over", { skip }, async () => {
    const id = CAMPAIGNS.decide;
    const m = machines();
    // Two units, each re-checked on the same machine with a different result.
    const u0 = await runOne(m, id);
    const u1 = await runOne(m, id);
    await srv.query(`UPDATE units SET special_at = special_at + 10 WHERE id = ${u1.lease.unit_id}`); // u0's re-check comes first
    for (const u of [u0, u1]) {
        await due(id);
        const f = await fetchFor(m, id);
        assert.equal(f.body.lease.unit_id, u.lease.unit_id);
        await submitFor(m, u.lease.unit_id, wrongFor(f.body.lease));
        const row = await unitRow(u.lease.unit_id);
        assert.deepEqual([row.state, row.recheck_kind], ["conflict", "same"]);
    }
    const d0 = digestOf(u0.lease);
    const decideFor = (body) => admin("POST", "/api/admin/decide", body);
    const missing = await decideFor({ unit_id: u0.lease.unit_id, digest: "0".repeat(64), note: "test" });
    assert.equal(missing.status, 400, missing.text);
    assert.match(missing.body.error, /no result of unit \d+ has digest/);
    const ok0 = await decideFor({ unit_id: u0.lease.unit_id, digest: d0, note: "reran on the CPU: the first result stands" });
    assert.equal(ok0.status, 200, ok0.text);
    assert.deepEqual(ok0.body, { ok: true, unit_id: u0.lease.unit_id, state: "accepted", digest: d0 });
    let row = await unitRow(u0.lease.unit_id);
    assert.deepEqual([row.state, row.checks, row.recheck, row.special_at, row.draw_at], ["accepted", 1, "failed", null, null]);
    assert.ok(row.accepted_at >= nowS() - 60, "a new embargo");
    const al0 = (await admin("GET", "/api/admin/alerts?all=1")).body.alerts.find((a) => a.unit_id === u0.lease.unit_id);
    assert.deepEqual([al0.outcome, al0.note, al0.resolved_at !== null, al0.needs], ["owner", "reran on the CPU: the first result stands", true, null]);
    assert.equal(await frontierOf(id), (BARINA_L0 + pow(32)).toString());
    const decided = (await ledgerOf(id)).slice(-1)[0];
    assert.deepEqual([decided.kind, decided.result, decided.who, decided.votes, decided.machine], ["block", d0, "owner", 1, m.hosts[0].slice(0, 8)]);

    const q = await adminScript(["decide", "--unit", String(u1.lease.unit_id), "--requeue", "--note", "verifier fixed; run it again"]);
    assert.equal(q.code, 0, q.stderr);
    assert.match(q.stdout, new RegExp(`unit ${u1.lease.unit_id}: requeued .*open for new results`));
    row = await unitRow(u1.lease.unit_id);
    assert.deepEqual([row.state, row.results, row.assigned, row.wanted, row.checks, row.recheck], ["open", 0, 0, 1, 0, "failed"]);
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM runs WHERE unit_id = ${u1.lease.unit_id}`))[0].n, 0);
    const again = await runOne(m, id);
    assert.deepEqual([again.lease.unit_id, again.submit.state], [u1.lease.unit_id, "accepted"], "run again from scratch");
    assert.deepEqual([await frontierOf(id), (await statsOf(id)).alerts_open], [(BARINA_L0 + 2n * pow(32)).toString(), 0]);

    assert.equal((await decideFor({ unit_id: u1.lease.unit_id, requeue: true, note: "x" })).status, 409, "an accepted unit waits for no decision");
    assert.equal((await decideFor({ unit_id: u1.lease.unit_id, note: "x" })).status, 400);
    assert.equal((await decideFor({ unit_id: u1.lease.unit_id, digest: d0, requeue: true, note: "x" })).status, 400);
    assert.equal((await decideFor({ unit_id: u1.lease.unit_id, requeue: true })).status, 400, "a note is required");
    assert.equal((await decideFor({ unit_id: 999999, requeue: true, note: "x" })).status, 404);
    assert.equal((await decideFor({ unit_id: 999999, digest: d0, note: "x" })).status, 404);
    assert.equal((await adminScript(["decide", "--unit", "1", "--note", "x"])).code, 2, "the script wants --digest, --requeue or --withdraw");
});

test("withdraw: an accepted block whose audit failed leaves the frontier with a 'withdrawn' entry, loses its runs and statistics, and runs again", { skip }, async () => {
    const id = CAMPAIGNS.withdraw; // 2^32..2^33
    const m = machines();
    const own = await runOne(m, id, { bits: 33 });
    const unitId = own.lease.unit_id;
    await runOne(m, id, { bits: 33 });
    assert.equal(await frontierOf(id), (BARINA_L0 + 2n * pow(33)).toString());
    const dig = digestOf(own.lease);
    const bad = await auditFor(m, unitId, dig, { samples: 400000, failures: 2, seconds: 90 });
    assert.equal(bad.status, 200, bad.text);
    const head = (await newApi().get("/api/ledger/head")).body;

    const w = await adminScript(["decide", "--unit", String(unitId), "--withdraw", "--note", "audit failed; rerun it"]);
    assert.equal(w.code, 0, w.stderr);
    assert.match(w.stdout, new RegExp(`unit ${unitId}: withdrawn from the frontier \\(its runs are deleted; it is open for new results\\)`));
    const u = await unitRow(unitId);
    assert.deepEqual([u.state, u.results, u.assigned, u.wanted, u.checks, u.digest, u.accepted_at], ["open", 0, 0, 1, 0, null, null]);
    assert.equal(await frontierOf(id), BARINA_L0.toString(), "the frontier rolls back to the block");
    for (const t of ["runs", "audits", "unit_results", "block_stats"]) {
        assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM ${t} WHERE unit_id = ${unitId}`))[0].n, 0, t);
    }
    const [done] = await srv.query(`SELECT kind, outcome, note, resolved_at FROM alerts WHERE unit_id = ${unitId}`);
    assert.deepEqual([done.kind, done.outcome, done.note, done.resolved_at !== null], ["audit", "requeued", "audit failed; rerun it", true]);
    const s = await statsOf(id);
    assert.deepEqual([s.totals.blocks, s.blocks.length, s.alerts_open, s.audits], [1, 1, 0, { passed: 0, failed: 1 }], "the failed audit stays counted");
    const entries = (await ledgerOf(id)).filter((e) => e.seq > head.seq);
    assert.deepEqual(entries.map((e) => [e.kind, e.start, e.bits, e.result, e.who, e.votes]), [["withdrawn", own.lease.l, 33, dig, "", 0]]);

    // The block runs again, and its result counts again.
    const again = await runOne(m, id, { bits: 33 });
    assert.deepEqual([again.lease.unit_id, again.submit.state], [unitId, "accepted"]);
    assert.equal(await frontierOf(id), (BARINA_L0 + 2n * pow(33)).toString());
    const back = (await ledgerOf(id)).filter((e) => e.seq > head.seq + 1);
    assert.deepEqual(back.map((e) => [e.kind, e.start, e.who]), [["block", own.lease.l, "owner"]]);

    // Only an accepted unit is withdrawn, and decide takes one choice.
    const f = await fetchFor(m, id, { bits: 33 });
    const open = await admin("POST", "/api/admin/decide", { unit_id: f.body.lease.unit_id, withdraw: true, note: "x" });
    assert.equal(open.status, 409, open.text);
    await releaseFor(m, f.body.lease.unit_id);
    assert.equal((await admin("POST", "/api/admin/decide", { unit_id: unitId, withdraw: 1, note: "x" })).status, 400);
    assert.equal((await admin("POST", "/api/admin/decide", { unit_id: unitId, withdraw: true, requeue: true, note: "x" })).status, 400);
    assert.equal((await admin("POST", "/api/admin/decide", { unit_id: unitId, requeue: true, note: "x" })).status, 409, "an accepted unit is withdrawn, not requeued");
    assert.equal((await adminScript(["decide", "--unit", String(unitId), "--withdraw", "--requeue", "--note", "x"])).code, 2);
});

test("requeue: a block in conflict starts over without its runs and audits, and a smaller fetch can then dissolve it", { skip }, async () => {
    const id = CAMPAIGNS.requeue; // 2^32..2^33, recheck_rate 1
    const m = machines();
    const own = await runOne(m, id, { bits: 33 });
    const unitId = own.lease.unit_id;
    const rep = await auditFor(m, unitId, digestOf(own.lease));
    assert.equal(rep.status, 200, rep.text);
    // The re-check disagrees: a conflict.
    await due(id);
    const rc = await fetchFor(m, id, { bits: 33 });
    assert.equal(rc.body.lease.unit_id, unitId);
    assert.equal((await submitFor(m, unitId, wrongFor(rc.body.lease))).status, 200);
    assert.equal((await unitRow(unitId)).state, "conflict");
    const rq = await admin("POST", "/api/admin/decide", { unit_id: unitId, requeue: true, note: "verifier fixed" });
    assert.deepEqual([rq.status, rq.body.state], [200, "open"], rq.text);
    for (const t of ["runs", "audits"]) {
        assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM ${t} WHERE unit_id = ${unitId}`))[0].n, 0, t);
    }
    // A fetch of 2^32: the 2^33 block without a result is dissolved, and its start leased.
    const small = await fetchFor(m, id, { bits: 32 });
    assert.equal(small.status, 200, small.text);
    assert.deepEqual([small.body.lease.k, small.body.lease.l], [32, own.lease.l]);
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM units WHERE id = ${unitId}`))[0].n, 0, "dissolved");
    await releaseFor(m, small.body.lease.unit_id);
});
