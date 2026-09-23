// npm test: lease expiry, the frontier, release and the lease caps, lost
// concurrent updates, records, and scripts/prune.sql over the units of these
// tests. helpers-server.mjs starts the Worker and holds the shared helpers.

import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import path from "node:path";
import { test } from "node:test";

import { CAMPAIGNS, FIX1, STATE_DIR, W, at, fetchFor, machines, off, releaseFor, resultFor, skip, srv, statsOf, submitFor, unitOf } from "./helpers-server.mjs";
import { SERVER_DIR, client } from "./helpers.mjs";

test("lease expiry: a result on a lapsed lease is refused until the lease is renewed", { skip }, async () => {
    const api = client(srv.url, "10.1.0.5");
    const id = CAMPAIGNS.expiry; // leases of 3 s
    const m = machines(3);
    const la = (await fetchFor(api, m, id, {}, 0)).body.lease;
    const lb = (await fetchFor(api, m, id, {}, 1)).body.lease;
    assert.deepEqual([off(la), off(lb)], [0, 1]);
    await new Promise((r) => setTimeout(r, 4200));
    const late = await submitFor(api, m, la.unit_id, resultFor(0));
    assert.equal(late.status, 409, late.text);
    assert.equal(late.body.lease_state, "expired");
    assert.equal((await unitOf(la.unit_id)).results, 0, "a refused result is not stored");

    // Fetching the unit again renews the lease while the unit takes a run.
    const back = (await fetchFor(api, m, id, { prefer_unit: la.unit_id }, 0)).body.lease;
    assert.equal(back.unit_id, la.unit_id);
    const ok = await submitFor(api, m, la.unit_id, resultFor(0));
    assert.equal(ok.status, 200, ok.text);
    assert.equal(ok.body.state, "accepted");
    const lc = (await fetchFor(api, m, id, {}, 2)).body.lease;
    assert.equal(lc.unit_id, lb.unit_id, "another machine gets the lowest unit with an expired lease");
    assert.equal((await submitFor(api, m, lc.unit_id, resultFor(1), 2)).body.state, "accepted");
    assert.equal((await statsOf(api, id)).frontier.value, at(2));
});

test("frontier: the contiguous accepted prefix", { skip }, async () => {
    const api = client(srv.url, "10.1.0.7");
    const id = CAMPAIGNS.gaps;
    const m = machines(3);
    const leases = [];
    for (let i = 0; i < 3; i++) {
        const a = (await fetchFor(api, m, id, {}, i)).body.lease;
        assert.equal(off(a), i);
        leases.push(a.unit_id);
    }
    const accept = async (i) => assert.equal((await submitFor(api, m, leases[i], resultFor(i), i)).body.state, "accepted");
    await accept(0);
    await accept(2);
    let st = await statsOf(api, id);
    assert.equal(st.totals.blocks, 2);
    assert.equal(st.frontier.value, at(1), "unit 2 is accepted but unit 1 is not");
    await accept(1);
    st = await statsOf(api, id);
    assert.equal(st.frontier.value, at(3));
    assert.equal(st.totals.numbers_verified, (3n * W).toString());
    assert.ok(Math.abs(st.frontier.log2 - Math.log2(2075 * 2 ** 60 + 3 * 2 ** 32)) < 1e-9);
});

test("release, the per-machine lease cap and the open-unit window", { skip }, async () => {
    const api = client(srv.url, "10.1.0.8");
    const id = CAMPAIGNS.cap;
    const m = machines(3);
    const first = (await fetchFor(api, m, id)).body.lease;
    const rel = await releaseFor(api, m, first.unit_id);
    assert.equal(rel.body.released, true);
    const gone = await submitFor(api, m, first.unit_id, resultFor(off(first)));
    assert.equal(gone.status, 409, "a released lease takes no result");
    assert.equal(gone.body.lease_state, "released");
    assert.equal((await fetchFor(api, m, id, {}, 1)).body.lease.unit_id, first.unit_id, "the released unit goes to the next fetch");
    // A machine holding 4 live leases (here of accepted units, which a fetch does not hand out again) gets no fifth.
    const now = Math.floor(Date.now() / 1000);
    await srv.sql(
        `WITH RECURSIVE i(x) AS (SELECT 100 UNION ALL SELECT x + 1 FROM i WHERE x < 103)
         INSERT INTO units (campaign_id, pos, bits, len, state, wanted, results, created_at, accepted_at) SELECT ${id}, x, 32, 1, 'accepted', 1, 1, 0, 0 FROM i;
         INSERT INTO runs (unit_id, user, host, state, leased_at, expires_at)
         SELECT id, 'owner', '${m.hosts[2]}', 'leased', ${now}, ${now + 3600} FROM units WHERE campaign_id = ${id} AND pos >= 100;`,
    );
    const capped = await fetchFor(api, m, id, {}, 2);
    assert.equal(capped.status, 429, capped.text);
    assert.match(capped.body.error, /this machine already holds 4 leases/);
    await srv.sql(`UPDATE runs SET state = 'released' WHERE host = '${m.hosts[2]}';`);

    const w = CAMPAIGNS.window; // max_open 1
    const wm = machines(2);
    assert.equal(off((await fetchFor(api, wm, w, {}, 0)).body.lease), 0);
    const none = await fetchFor(api, wm, w, {}, 1);
    assert.equal(none.status, 200);
    assert.equal(none.body.lease, null, "max_open = 1: no second unit opens before the first is accepted");
    assert.ok(none.body.retry_after > 0);
});

test("submit: a lost concurrent update is rolled back and retried; the same result sent twice at once answers duplicate", { skip }, async () => {
    const api = client(srv.url, "10.1.0.9");
    const id = CAMPAIGNS.race;
    const m = machines(3);
    for (const [mode, idx] of [["1", 0], ["2", 1]]) {
        const la = (await fetchFor(api, m, id, {}, idx)).body.lease;
        assert.equal(off(la), idx);
        const sa = await submitFor(api, m, la.unit_id, resultFor(idx), idx, { "x-test-race": mode });
        assert.equal(sa.status, 200, sa.text);
        assert.equal(sa.body.state, "accepted", `mode ${mode}`);
        const unit = await unitOf(la.unit_id);
        assert.deepEqual([unit.results, unit.assigned], [1, 1], `mode ${mode}: the rolled-back attempt must leave no write behind`);
    }
    const lc = (await fetchFor(api, m, id, {}, 2)).body.lease;
    const twice = await submitFor(api, m, lc.unit_id, resultFor(off(lc)), 2, { "x-test-submit-twice": "1" });
    assert.deepEqual([twice.status, twice.body.duplicate], [200, true], twice.text);
    const u = await unitOf(lc.unit_id);
    assert.deepEqual([u.results, u.state], [1, "accepted"], "counted once");
    const st = await statsOf(api, id);
    assert.deepEqual([st.totals.blocks, st.frontier.value], [3, at(3)]);
});

test("records above the threshold are stored with the accepted unit", { skip }, async () => {
    const api = client(srv.url, "10.1.0.10");
    const id = CAMPAIGNS.records;
    const m = machines();
    const la = (await fetchFor(api, m, id)).body.lease;
    assert.equal((await submitFor(api, m, la.unit_id, FIX1)).body.state, "accepted");
    const rows = await srv.query(`SELECT n, peak, unit_id, users FROM records WHERE campaign_id = ${id}`);
    const byN = Object.fromEntries(rows.map((r) => [r.n, r.peak]));
    assert.equal(byN["274133054632352106267"], "56649062372194325899121269007146717645316");
    assert.equal(rows.length, 3);
    assert.ok(rows.every((r) => r.unit_id === la.unit_id && r.users === "owner"));
    // All three lie below #97 and its peak: no path record beyond it.
    const ps = await statsOf(api, id);
    assert.deepEqual(ps.path_records.map((r) => r.number), [97]);
});

// Runs last: it archives every accepted unit of every campaign.
test("scripts/prune.mjs runs prune.sql: it archives documents and keeps digests, records and block statistics", { skip }, async () => {
    const api = client(srv.url, "10.1.0.19");
    const unitId = (await srv.query(`SELECT id FROM units WHERE campaign_id = ${CAMPAIGNS.records} AND pos = 0`))[0].id;
    const count = async (sql) => (await srv.query(sql))[0].n;
    assert.equal(await count(`SELECT COUNT(*) AS n FROM unit_results WHERE unit_id = ${unitId}`), 1);
    const before = await unitOf(unitId);
    const statsBefore = await statsOf(api, CAMPAIGNS.records);
    // --days 0 archives the units accepted before this second; the test database, without a backup.
    await new Promise((r) => setTimeout(r, 1100));
    const args = ["--database", "DB", "--days", "0", "--no-backup", "--", "--local", "--config", "wrangler.example.jsonc", "--env", "test", "--persist-to", STATE_DIR];
    const run = await new Promise((resolve) => {
        execFile(process.execPath, [path.join(SERVER_DIR, "scripts", "prune.mjs"), ...args], { cwd: SERVER_DIR }, (err, stdout, stderr) =>
            resolve({ code: err ? err.code : 0, stdout, stderr }),
        );
    });
    assert.equal(run.code, 0, run.stdout + run.stderr);
    assert.match(run.stdout, /pruned DB: documents of blocks accepted more than 0 days ago are archived/);
    const after = await unitOf(unitId);
    assert.deepEqual([after.state, after.digest], ["accepted", before.digest]);
    assert.equal(await count(`SELECT COUNT(*) AS n FROM unit_results WHERE unit_id = ${unitId}`), 0, "the document is archived");
    assert.deepEqual(await srv.query(`SELECT digest FROM runs WHERE unit_id = ${unitId}`), [{ digest: before.digest }], "the run's digest stays");
    assert.equal(await count(`SELECT COUNT(*) AS n FROM records WHERE campaign_id = ${CAMPAIGNS.records}`), 3);
    const statsAfter = await statsOf(api, CAMPAIGNS.records);
    assert.deepEqual(
        [statsAfter.totals, statsAfter.max_excursion, statsAfter.path_records],
        [statsBefore.totals, statsBefore.max_excursion, statsBefore.path_records],
    );
    const lapsed = await count("SELECT COUNT(*) AS n FROM runs r JOIN units u ON u.id = r.unit_id WHERE u.state = 'accepted' AND r.state <> 'submitted'");
    assert.equal(lapsed, 0);
});
