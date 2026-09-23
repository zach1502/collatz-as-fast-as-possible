// npm test: random re-checks, their preference order, pacing and embargo, and
// the rows a fetch reads while many re-checks wait. The Worker and the helpers
// are in test/helpers-campaign.mjs.

import assert from "node:assert/strict";
import { test } from "node:test";

import { BARINA_L0 } from "../scripts/campaign.mjs";
import {
    CAMPAIGNS,
    DAY,
    activate,
    campaignRow,
    due,
    fetchFor,
    machines,
    measure,
    nowS,
    onGpu,
    pow,
    releaseFor,
    resultFor,
    runOne,
    serveCampaigns,
    skip,
    srv,
    submitFor,
    unitRow,
} from "./helpers-campaign.mjs";
import { leaseShape } from "./helpers.mjs";

serveCampaigns();

test("re-checks: queued at the campaign's rate; a matching one passes and adds no vote; the worker cannot tell", { skip }, async () => {
    // recheck_rate 0, then 1: the draw, a random fraction in [0, 1), is never below 0 and always below 1.
    const id = CAMPAIGNS.rate;
    const m = machines();
    const u0 = await runOne(m, id);
    assert.equal((await unitRow(u0.lease.unit_id)).recheck, null, "rate 0: not drawn");
    await srv.query(`UPDATE campaigns SET recheck_rate = 1 WHERE id = ${id}`);
    const u1 = await runOne(m, id);
    measure("submit (accepts, queues a re-check)", u1.submitRes);
    const row1 = await unitRow(u1.lease.unit_id);
    assert.equal(row1.recheck, "pending");
    assert.ok(row1.special_at > 0);
    // Two regular results (RECHECK_EVERY): the next fetch is the re-check.
    const f = await fetchFor(m, id);
    measure("fetch (re-check lease)", f);
    assert.equal(f.body.lease.unit_id, u1.lease.unit_id);
    assert.deepEqual(leaseShape(f.body.lease), leaseShape(u1.lease), "a re-check lease has the fields of any lease");
    assert.equal(f.body.redelivered, undefined);
    assert.deepEqual(
        [f.body.lease.h, f.body.lease.k, f.body.lease.a, f.body.lease.b, f.body.lease.note, f.body.lease.backend],
        [u1.lease.h, u1.lease.k, u1.lease.a, u1.lease.b, null, "cpu"],
        "the same block and parameters as the original run",
    );
    // A released re-check is renewed by prefer_unit, as a re-check.
    await releaseFor(m, u1.lease.unit_id);
    const back = await fetchFor(m, id, { prefer_unit: u1.lease.unit_id });
    assert.equal(back.body.lease.unit_id, u1.lease.unit_id);
    assert.deepEqual(leaseShape(back.body.lease), leaseShape(u1.lease));
    const s = await submitFor(m, f.body.lease.unit_id, resultFor(f.body.lease));
    measure("submit (re-check matches)", s);
    assert.equal(s.status, 200, s.text);
    assert.deepEqual(s.body, u1.submit, "the answer of a regular accepting result");
    const row = await unitRow(u1.lease.unit_id);
    assert.deepEqual(
        [row.state, row.checks, row.recheck, row.recheck_kind, row.special_at],
        ["accepted", 1, "passed", "same", null],
        "a second run of the block is no second vote",
    );
    const c = await campaignRow(id);
    assert.deepEqual([c.units_accepted, c.units_twice, c.rechecks_passed, c.rechecks_failed, c.rechecks_same], [2, 0, 1, 0, 1]);
    const runs = await srv.query(`SELECT role FROM runs WHERE unit_id = ${u1.lease.unit_id} AND state = 'submitted' ORDER BY id`);
    assert.deepEqual(runs.map((r) => r.role), ["regular", "recheck"]);
});

test("re-check preference: another active host before the same host", { skip }, async () => {
    const id = CAMPAIGNS.prefHost;
    const m = machines(2);
    await activate(id, m, 1);
    const a = await runOne(m, id, {}, 0);
    await due(id);
    const same = await fetchFor(m, id, {}, 0);
    assert.notEqual(same.body.lease.unit_id, a.lease.unit_id, "host 0 waits: the active host 1 is a better re-checker");
    await releaseFor(m, same.body.lease.unit_id, 0);
    const f = await fetchFor(m, id, {}, 1);
    assert.equal(f.body.lease.unit_id, a.lease.unit_id);
    await submitFor(m, a.lease.unit_id, resultFor(f.body.lease), 1);
    const row = await unitRow(a.lease.unit_id);
    assert.deepEqual([row.recheck, row.recheck_kind, row.checks], ["passed", "host", 1]);
});

test("re-check preference: on the same host, the other backend (a GPU result re-checked on the CPU with recheck_cpu)", { skip }, async () => {
    const id = CAMPAIGNS.prefBackend;
    const m = machines();
    const a = await runOne(m, id, { backend: "gpu" }, 0, onGpu);
    assert.equal(a.lease.backend, "gpu");
    assert.equal((await srv.query(`SELECT backend FROM runs WHERE unit_id = ${a.lease.unit_id}`))[0].backend, "gpu");
    await due(id);
    const f = await fetchFor(m, id, { backend: "gpu", recheck_cpu: true });
    assert.equal(f.body.lease.unit_id, a.lease.unit_id);
    assert.equal(f.body.lease.backend, "cpu", "the lease asks for the CPU");
    await submitFor(m, a.lease.unit_id, resultFor(f.body.lease));
    const row = await unitRow(a.lease.unit_id);
    assert.deepEqual([row.recheck, row.recheck_kind], ["passed", "backend"]);
});

test("re-check preference: last, the same host and backend; inactive hosts do not count", { skip }, async () => {
    const id = CAMPAIGNS.prefSame;
    const m = machines(2);
    // Host 1 was last seen two days ago: not active, so it does not hold the re-check back.
    await activate(id, m, 1);
    await srv.query(`UPDATE hosts SET last_seen = last_seen - ${2 * DAY} WHERE campaign_id = ${id} AND host = '${m.hosts[1]}'`);
    const a = await runOne(m, id, { backend: "gpu" }, 0, onGpu);
    await due(id);
    const f = await fetchFor(m, id, { backend: "gpu" });
    assert.deepEqual([f.body.lease.unit_id, f.body.lease.backend], [a.lease.unit_id, "gpu"]);
    await submitFor(m, a.lease.unit_id, onGpu(f.body.lease));
    assert.equal((await unitRow(a.lease.unit_id)).recheck_kind, "same");
    assert.equal((await campaignRow(id)).rechecks_same, 1);
});

test("pacing: a re-check waits for RECHECK_EVERY regular results unless it has waited a day", { skip }, async () => {
    const id = CAMPAIGNS.pacing;
    const m = machines();
    const u0 = await runOne(m, id);
    const u1 = await runOne(m, id);
    assert.notEqual(u1.lease.unit_id, u0.lease.unit_id, "one regular result so far: no re-check yet");
    const r0 = await runOne(m, id);
    assert.equal(r0.lease.unit_id, u0.lease.unit_id, "after two, the oldest re-check");
    const u2 = await runOne(m, id);
    assert.ok(![u0.lease.unit_id, u1.lease.unit_id].includes(u2.lease.unit_id), "the count starts again");
    await srv.query(`UPDATE units SET special_at = special_at - ${DAY + 1} WHERE id = ${u1.lease.unit_id}`);
    const r1 = await fetchFor(m, id);
    assert.equal(r1.body.lease.unit_id, u1.lease.unit_id, "a re-check that waited a day goes out at once");
    await releaseFor(m, r1.body.lease.unit_id);
});

test("pacing: a machine without regular work gets a waiting re-check at once", { skip }, async () => {
    const id = CAMPAIGNS.idle; // the campaign ends after one block
    const m = machines();
    const u0 = await runOne(m, id);
    const f = await fetchFor(m, id);
    assert.equal(f.body.lease?.unit_id, u0.lease.unit_id, f.text);
    await submitFor(m, u0.lease.unit_id, resultFor(f.body.lease));
    const none = await fetchFor(m, id);
    assert.equal(none.body.lease, null);
    assert.match(none.body.reason, /campaign is full/);
});

test("embargo: a re-check is offered only while its lease can end before the embargo, and closes at it", { skip }, async () => {
    const id = CAMPAIGNS.embargo; // recheck_rate 1, leases of 3600 s, EMBARGO_SECONDS 604800
    const [EMBARGO, LEASE] = [604800, 3600];
    const age = (unitId, s) => srv.query(`UPDATE units SET accepted_at = accepted_at - ${s} WHERE id = ${unitId}`);
    const m = machines();
    const u0 = (await runOne(m, id)).lease;
    const u1 = (await runOne(m, id)).lease;
    // Unit 0 was accepted EMBARGO - LEASE + 60 s ago: a re-check lease would outlast the embargo.
    await age(u0.unit_id, EMBARGO - LEASE + 60);
    await due(id);
    const f1 = await fetchFor(m, id);
    assert.equal(f1.body.lease.unit_id, u1.unit_id, "unit 0's re-check is no longer offered");
    // The embargo of unit 1 ends while its re-check lease is live: the result is refused.
    await age(u1.unit_id, EMBARGO + 1);
    const late = await submitFor(m, u1.unit_id, resultFor(u1));
    assert.equal(late.status, 409, late.text);
    assert.equal(late.body.lease_state, "expired");
    assert.match(late.body.error, /your lease of unit \d+ expired/, "the answer of any lapsed lease");
    const f2 = await fetchFor(m, id);
    assert.equal(f2.body.redelivered, undefined, "a closed re-check is not handed out again");
    assert.notEqual(f2.body.lease.unit_id, u1.unit_id);
    await releaseFor(m, f2.body.lease.unit_id);
    // Unit 0's re-check has waited lease_seconds with no lease: the next fetch expires it.
    await srv.query(`UPDATE units SET draw_at = draw_at - ${LEASE + 1} WHERE id = ${u0.unit_id}`);
    const f3 = await fetchFor(m, id);
    await releaseFor(m, f3.body.lease.unit_id);
    const row0 = await unitRow(u0.unit_id);
    assert.deepEqual([row0.recheck, row0.special_at, row0.draw_at], ["expired", null, null]);
});

test("fetch: rows read stay bounded in a mixed-size campaign with many small units and waiting re-checks", { skip }, async () => {
    const id = CAMPAIGNS.small; // blocks of 2^32 to 2^42: up to 1024 slots
    const m = machines(2);
    const now = nowS();
    // 2,000 accepted blocks of 2^32, each waiting for its re-check, the oldest at the highest position.
    await srv.sql(
        `WITH RECURSIVE i(x) AS (SELECT 0 UNION ALL SELECT x + 1 FROM i WHERE x < 1999)
         INSERT INTO units (campaign_id, pos, bits, len, state, wanted, results, checks, digest, created_at, accepted_at,
                            recheck, special_at, draw_at)
         SELECT ${id}, x, 32, 1, 'accepted', 1, 1, 1, 'd', ${now}, ${now}, 'pending', ${now} - x, ${now} FROM i;
         UPDATE campaigns SET frontier_pos = 2000, alloc_pos = 2000 WHERE id = ${id};`,
    );
    const f = await fetchFor(m, id, { bits: 42 });
    assert.equal(f.status, 200, f.text);
    assert.deepEqual([f.body.lease.k, f.body.lease.l], [42, (BARINA_L0 + 2048n * pow(32)).toString()]);
    assert.ok(f.rowsRead < 100, `fetch read ${f.rowsRead} rows`);
    measure("fetch (2^42 after 2,000 small units)", f);
    // The re-check that waited the longest goes out, with as few rows read.
    await due(id);
    const g = await fetchFor(m, id, { bits: 32 }, 1);
    assert.equal(g.status, 200, g.text);
    const oldest = (await srv.query(`SELECT id FROM units WHERE campaign_id = ${id} AND pos = 1999`))[0].id;
    assert.equal(g.body.lease.unit_id, oldest);
    assert.ok(g.rowsRead < 100, `fetch read ${g.rowsRead} rows`);
    measure("fetch (re-check behind 2,000 waiting ones)", g);
    await releaseFor(m, f.body.lease.unit_id);
    await releaseFor(m, oldest, 1);
});
