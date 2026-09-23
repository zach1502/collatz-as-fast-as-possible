// npm test: a result counts only on the lease it is sent on. A result sent
// again after its unit was started over, after another machine took the lease
// or while the unit's re-check is out lands on no other lease; a copy of a
// submitted result answers duplicate, and a differing one is kept for the
// owner with one alert; release hands back only the caller's lease. Also the
// verifier's block range in the allocator and in scripts/campaign.mjs. The
// Worker and the helpers are in test/helpers-campaign.mjs.

import assert from "node:assert/strict";
import { test } from "node:test";

import { campaignSql } from "../scripts/campaign.mjs";
import {
    CAMPAIGNS,
    RANGE_L0,
    TOKEN,
    activate,
    admin,
    adminScript,
    digestOf,
    due,
    fetchFor,
    machines,
    measure,
    nowS,
    releaseFor,
    resultFor,
    runOne,
    serveCampaigns,
    skip,
    srv,
    statsOf,
    submitBody,
    submitFor,
    unitRow,
    wrongFor,
} from "./helpers-campaign.mjs";

serveCampaigns();

const alertsOf = async (unitId) => (await admin("GET", "/api/admin/alerts?all=1")).body.alerts.filter((a) => a.unit_id === unitId);

test("a wrong result sent again after its unit was started over is not taken on the new lease, and the right one is", { skip }, async () => {
    const id = CAMPAIGNS.leaseRequeue; // recheck_rate 1
    const m = machines(3);
    const u = (await fetchFor(m, id, {}, 0)).body.lease;
    assert.equal((await submitFor(m, u.unit_id, wrongFor(u), 0)).body.state, "accepted");
    await activate(id, m, 1);
    await due(id);
    const b = (await fetchFor(m, id, {}, 1)).body.lease;
    assert.equal(b.unit_id, u.unit_id, "machine 1 gets the re-check");
    assert.equal((await submitFor(m, u.unit_id, resultFor(b), 1)).status, 200);
    assert.equal((await unitRow(u.unit_id)).state, "conflict");
    const rq = await admin("POST", "/api/admin/decide", { unit_id: u.unit_id, requeue: true, note: "start it over" });
    assert.equal(rq.status, 200, rq.text);
    const c = (await fetchFor(m, id, {}, 2)).body.lease;
    assert.equal(c.unit_id, u.unit_id, "machine 2 leases the unit again");

    // Machine 0 sends its wrong result again on its old lease, which the requeue deleted.
    const late = await m.api.post("/api/submit", submitBody(m, u.unit_id, wrongFor(u), 0, u.lease_id));
    measure("submit (on a lease the requeue deleted)", late);
    assert.equal(late.status, 409, late.text);
    assert.match(late.body.error, /does not exist/);
    assert.equal(late.body.lease_state, undefined, "the worker drops the result instead of asking for the unit again");
    assert.deepEqual([(await unitRow(u.unit_id)).state, (await unitRow(u.unit_id)).results], ["open", 0], "nothing was taken");
    const ok = await submitFor(m, u.unit_id, resultFor(c), 2);
    assert.deepEqual([ok.status, ok.body.state], [200, "accepted"], ok.text);
    assert.equal((await unitRow(u.unit_id)).digest, digestOf(c), "the unit holds the right result");
    assert.equal(new Set([u.lease_id, b.lease_id, c.lease_id]).size, 3, "the run, its re-check and the run after the requeue are three leases");
});

test("a copy of an accepted result sent while the re-check is out answers duplicate and passes no re-check", { skip }, async () => {
    const id = CAMPAIGNS.leaseRecheck; // recheck_rate 1
    const m = machines(2);
    const own = await runOne(m, id);
    await activate(id, m, 1);
    await due(id);
    const rc = (await fetchFor(m, id, {}, 1)).body.lease;
    assert.equal(rc.unit_id, own.lease.unit_id);
    const again = await submitFor(m, own.lease.unit_id, resultFor(own.lease), 0);
    measure("submit (copy of an accepted result)", again);
    assert.deepEqual([again.status, again.body], [200, { duplicate: true }], again.text);
    let row = await unitRow(own.lease.unit_id);
    assert.deepEqual([row.recheck, row.recheck_kind, row.results], ["pending", null, 1], "the copy is no re-check");
    // The re-check that disagrees still counts, with its alert.
    assert.equal((await submitFor(m, own.lease.unit_id, wrongFor(rc), 1)).status, 200);
    row = await unitRow(own.lease.unit_id);
    assert.deepEqual([row.state, row.recheck, row.recheck_kind], ["conflict", "failed", "host"]);
    const [al] = await alertsOf(own.lease.unit_id);
    assert.deepEqual([al.kind, al.fields], ["host", ["blocks_total"]]);
});

test("a result on a lease that ran out and went to another machine is not taken, and release hands back only one's own lease", { skip }, async () => {
    const id = CAMPAIGNS.leaseTaken;
    const m = machines(2);
    const a = (await fetchFor(m, id, {}, 0)).body.lease;
    await srv.query(`UPDATE runs SET expires_at = ${nowS() - 1} WHERE unit_id = ${a.unit_id}`);
    const b = (await fetchFor(m, id, {}, 1)).body.lease;
    assert.deepEqual([b.unit_id, b.lease_id], [a.unit_id, a.lease_id], "machine 1 renews the run that ran out: the same lease id");
    const rel = await releaseFor(m, a.unit_id, 0);
    assert.deepEqual([rel.status, rel.body.released], [200, false], "machine 0 cannot hand back machine 1's lease");
    const late = await submitFor(m, a.unit_id, resultFor(a), 0);
    assert.equal(late.status, 409, late.text);
    assert.equal(late.body.lease_state, "expired");
    assert.match(late.body.error, /went to another machine/);
    const [run] = await srv.query(`SELECT host, state, digest FROM runs WHERE unit_id = ${a.unit_id}`);
    assert.deepEqual([run.host, run.state, run.digest], [m.hosts[1], "leased", null], "machine 1 still holds the lease");
    const ok = await submitFor(m, a.unit_id, resultFor(b), 1);
    assert.deepEqual([ok.status, ok.body.state], [200, "accepted"], ok.text);
    assert.deepEqual((await submitFor(m, a.unit_id, resultFor(a), 0)).body, { duplicate: true }, "the same result from machine 0 is already there");
});

test("a different result sent on a lease that took one is kept with one alert per digest and does not count", { skip }, async () => {
    const id = CAMPAIGNS.leaseDiffer;
    const m = machines();
    const own = await runOne(m, id);
    const unitId = own.lease.unit_id;
    const first = await submitFor(m, unitId, wrongFor(own.lease));
    measure("submit (differing result, kept with an alert)", first);
    assert.equal(first.status, 409, first.text);
    assert.equal(first.body.rejected, true);
    assert.match(first.body.error, /you already submitted a different result .*kept for the owner \(alert \d+\)/);
    const second = await submitFor(m, unitId, wrongFor(own.lease));
    assert.deepEqual([second.status, second.body.alert_id], [409, first.body.alert_id], "the same digest again: the same alert");
    const alerts = await alertsOf(unitId);
    assert.equal(alerts.length, 1, "one alert");
    const [al] = alerts;
    assert.deepEqual([al.id, al.kind, al.fields, al.resolved_at], [first.body.alert_id, "resend", ["blocks_total"], null]);
    assert.deepEqual(
        al.runs.map((r) => [r.role, r.host, r.digest]),
        [["regular", m.hosts[0], digestOf(own.lease)], ["resend", m.hosts[0], JSON.parse(wrongFor(own.lease)).result.result_digest]],
    );
    assert.equal(al.runs[1].lease_id, own.lease.lease_id);
    assert.equal(JSON.parse(al.runs[1].result_json).result.blocks_total, JSON.parse(wrongFor(own.lease)).result.blocks_total, "the document is kept");
    const row = await unitRow(unitId);
    assert.deepEqual([row.state, row.results, row.digest], ["accepted", 1, digestOf(own.lease)], "the unit is unchanged");
    assert.equal((await statsOf(id)).alerts_open, 1);
    const listed = await adminScript(["alerts"]);
    assert.match(listed.stdout, new RegExp(`unit ${unitId} .*a result sent on no live lease; differs in blocks_total`));
});

test("submit and release without a lease_id are refused (a worker from before lease ids)", { skip }, async () => {
    const id = CAMPAIGNS.leaseNone;
    const m = machines();
    const l = (await fetchFor(m, id)).body.lease;
    const sub = await m.api.post("/api/submit", submitBody(m, l.unit_id, resultFor(l)).replace(/,"lease_id":\d+/, ""));
    assert.equal(sub.status, 400, sub.text);
    assert.match(sub.body.error, /lease_id is missing/);
    const rel = await m.api.post("/api/release", { token: TOKEN, host: m.hosts[0], unit_id: l.unit_id });
    assert.equal(rel.status, 400, rel.text);
    assert.equal((await unitRow(l.unit_id)).results, 0);
    assert.equal((await releaseFor(m, l.unit_id)).body.released, true);
});

test("the allocator hands out no block past the verifier's range, also when the first block of the size fits", { skip }, async () => {
    const id = CAMPAIGNS.verifierRange; // blocks of 2^60 at A = 10: M = 50
    const m = machines(2);
    const first = await fetchFor(m, id, { bits: 60 }, 0);
    assert.equal(first.status, 200, first.text);
    const end = BigInt(first.body.lease.end);
    assert.ok(end * 3n ** 50n < 1n << 175n, "the first block is inside the range");
    const next = await fetchFor(m, id, { bits: 60 }, 1);
    assert.equal(next.status, 200, next.text);
    assert.equal(next.body.lease, null, `the next block ends at ${end + (1n << 60n)}, outside the range`);
    await releaseFor(m, first.body.lease.unit_id);

    // campaign.mjs checks the last block of every size when the campaign has an end.
    const o = { name: "x", minBits: 59, maxBits: 60, A: 10, commits: ["abc"], L0: (RANGE_L0 - (1n << 60n)).toString() };
    assert.doesNotThrow(() => campaignSql({ ...o, end: (RANGE_L0 + (1n << 60n)).toString() }));
    assert.throws(() => campaignSql({ ...o, end: (RANGE_L0 + (2n << 60n)).toString() }), /last block of 2\^60 numbers/);
});
