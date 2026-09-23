// npm test: the worker token, acceptance of every block on its one result,
// and the audits a machine reports. The Worker and the helpers are in
// test/helpers-owner.mjs.

import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import path from "node:path";
import { test } from "node:test";

import { BARINA_L0 } from "../scripts/campaign.mjs";
import {
    ADMIN_SECRET,
    CAMPAIGNS,
    COMMIT,
    TOKEN,
    fetchAs,
    measure,
    newApi,
    pow,
    resultFor,
    runOne,
    serveOwner,
    skip,
    srv,
    statsOf,
    submitAs,
} from "./helpers-owner.mjs";
import { SERVER_DIR, newHost, parseExact, stringifyExact } from "./helpers.mjs";

serveOwner();

test("fetch, submit and release need the worker token", { skip }, async () => {
    const api = newApi();
    const host = newHost();
    const id = CAMPAIGNS.auth;
    const refused = async (r, what) => {
        assert.equal(r.status, 401, `${what}: ${r.text}`);
        assert.match(r.body.error, /--token-file/);
    };
    await refused(await api.post("/api/fetch", { host, commit: COMMIT, campaign: id }), "no token");
    await refused(await fetchAs(api, host, id, {}, "wrong-token-0123456789abcdef0123456789"), "wrong token");
    await refused(await fetchAs(api, host, id, {}, TOKEN.toUpperCase()), "token in another case");
    await refused(await fetchAs(api, host, id, {}, `${TOKEN} `), "token with a trailing space");
    await refused(await fetchAs(api, host, id, {}, ""), "empty token");
    await refused(await api.post("/api/fetch", { token: 12345, host, commit: COMMIT, campaign: id }), "a number");
    assert.equal((await fetchAs(api, "not-a-host", id)).status, 400, "the host id is still checked");
    const f = await fetchAs(api, host, id);
    assert.equal(f.status, 200, f.text);
    measure("fetch (opens a block)", f);
    const lease = f.body.lease;
    await refused(await submitAs(api, host, lease.unit_id, resultFor(lease), "x".repeat(40)), "submit, wrong token");
    await refused(await api.post("/api/release", { host, unit_id: lease.unit_id, lease_id: lease.lease_id }), "release, no token");
    const [stored] = await srv.query(`SELECT results FROM units WHERE id = ${lease.unit_id}`);
    assert.equal(stored.results, 0, "a refused submit stores nothing");
    const rel = await api.post("/api/release", { token: TOKEN, host, unit_id: lease.unit_id, lease_id: lease.lease_id });
    assert.deepEqual([rel.status, rel.body.released], [200, true]);
});

test("every block is accepted on its one result; several machines work at once; the result checks stay", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.accept;
    const [h1, h2] = [newHost(), newHost()];
    const a = await fetchAs(api, h1, id, { bits: 33 });
    const b = await fetchAs(api, h2, id, { bits: 33 });
    assert.notEqual(a.body.lease.unit_id, b.body.lease.unit_id, "two machines get two blocks");
    const again = await fetchAs(api, h1, id, { bits: 33 });
    assert.deepEqual([again.body.lease.unit_id, again.body.redelivered], [a.body.lease.unit_id, true]);
    const bad = await submitAs(api, h1, a.body.lease.unit_id, resultFor(a.body.lease, (d) => (d.params.B = 25)));
    assert.equal(bad.status, 422, "the parameters must still match the lease");
    const forged = parseExact(resultFor(a.body.lease));
    forged.result.result_digest = "0".repeat(64);
    assert.equal((await submitAs(api, h1, a.body.lease.unit_id, stringifyExact(forged))).status, 422, "the digest is recomputed");
    const other = await submitAs(api, h1, a.body.lease.unit_id, resultFor(a.body.lease, (d) => (d.commit = "0123abc")));
    assert.equal(other.status, 422, "the commit allow-list holds");
    const sa = await submitAs(api, h1, a.body.lease.unit_id, resultFor(a.body.lease));
    measure("submit (accepts, with block stats)", sa);
    assert.deepEqual([sa.body.state, sa.body.verdict], ["accepted", "accepted"]);
    const sb = await submitAs(api, h2, b.body.lease.unit_id, resultFor(b.body.lease));
    assert.equal(sb.body.state, "accepted");
    const st = (await statsOf(id)).body;
    assert.equal(st.totals.blocks, 2);
    assert.equal(st.frontier.value, (BARINA_L0 + pow(34)).toString());
    // The lease cap counts per machine: five machines hold a lease each (the cap is 4).
    const many = [];
    for (let i = 0; i < 5; i++) {
        many.push(await fetchAs(api, newHost(), CAMPAIGNS.hosts));
    }
    assert.deepEqual(many.map((r) => r.status), [200, 200, 200, 200, 200]);
    assert.equal(new Set(many.map((r) => r.body.lease?.unit_id)).size, 5);
    const q2 = await fetchAs(api, h1, CAMPAIGNS.q2);
    assert.equal(q2.status, 500, q2.text);
    assert.match(q2.body.error, /quorum 2 is not 1/);
});

test("audits: a machine reports the audit of its own result; the page counts it, and one with failures raises an alert", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.audits;
    const host = newHost();
    const a = await runOne(api, host, id);
    const b = await runOne(api, host, id);
    const runOf = async (unitId) => (await srv.query(`SELECT digest, backend FROM runs WHERE unit_id = ${unitId} AND state = 'submitted'`))[0];
    const [ra, rb] = [await runOf(a.lease.unit_id), await runOf(b.lease.unit_id)];
    const report = (unitId, seed, o = {}, h = host) =>
        api.post("/api/audit", { token: TOKEN, host: h, unit_id: unitId, seed, samples: 16384, failures: 0, seconds: 3.25, ...o });

    const noToken = await api.post("/api/audit", { host, unit_id: a.lease.unit_id, seed: ra.digest, samples: 16384, failures: 0, seconds: 1 });
    assert.equal(noToken.status, 401, noToken.text);
    for (const [o, what] of [
        [{ seed: "x" }, "a seed that is no digest"],
        [{ seed: ra.digest.toUpperCase() }, "a digest in upper case"],
        [{ samples: 0 }, "no residues"],
        [{ samples: 10000001 }, "more residues than an audit draws"],
        [{ failures: -1 }, "negative failures"],
        [{ failures: 1.5 }, "a fraction of a failure"],
        [{ seconds: "3" }, "seconds as text"],
        [{ seconds: -1 }, "negative seconds"],
        [{ unit_id: "1" }, "the unit id as text"],
    ]) {
        const r = await report(a.lease.unit_id, ra.digest, o);
        assert.equal(r.status, 400, `${what}: ${r.text}`);
    }
    // Only the audit of a result this machine submitted, seeded with its digest.
    for (const [r, what] of [
        [await report(a.lease.unit_id, rb.digest), "another result's digest"],
        [await report(a.lease.unit_id, ra.digest, {}, newHost()), "another machine"],
        [await report(999999, ra.digest), "no such unit"],
    ]) {
        assert.equal(r.status, 409, `${what}: ${r.text}`);
    }

    const ok = await report(a.lease.unit_id, ra.digest);
    assert.equal(ok.status, 200, ok.text);
    const again = await report(a.lease.unit_id, ra.digest);
    assert.deepEqual([again.status, again.body.duplicate], [200, true], "the same report twice counts once");
    const other = await report(a.lease.unit_id, ra.digest, { failures: 3 });
    assert.equal(other.status, 409, `other figures for the same audit: ${other.text}`);
    let s = (await statsOf(id)).body;
    assert.deepEqual([s.audits, s.alerts_open], [{ passed: 1, failed: 0 }, 0]);

    const bad = await report(b.lease.unit_id, rb.digest, { samples: 400000, failures: 2, seconds: 97.3 });
    assert.equal(bad.status, 200, bad.text);
    s = (await statsOf(id)).body;
    assert.deepEqual([s.audits, s.alerts_open], [{ passed: 1, failed: 1 }, 1]);
    assert.deepEqual([s.totals.blocks, s.frontier.value], [2, (BARINA_L0 + 2n * pow(32)).toString()], "the blocks stay accepted");
    assert.equal((await srv.query(`SELECT state FROM units WHERE id = ${b.lease.unit_id}`))[0].state, "accepted");
    assert.deepEqual(await srv.query(`SELECT unit_id, user, host, seed, samples, failures, seconds FROM audits WHERE campaign_id = ${id} ORDER BY id`), [
        { unit_id: a.lease.unit_id, user: "owner", host, seed: ra.digest, samples: 16384, failures: 0, seconds: 3.25 },
        { unit_id: b.lease.unit_id, user: "owner", host, seed: rb.digest, samples: 400000, failures: 2, seconds: 97.3 },
    ]);
    const auth = { authorization: `Bearer ${ADMIN_SECRET}` };
    const [alert, ...more] = (await api.get("/api/admin/alerts", auth)).body.alerts.filter((x) => x.campaign_id === id);
    assert.equal(more.length, 0, "one alert: the audit with failures");
    assert.deepEqual([alert.unit_id, alert.kind, alert.fields, alert.outcome, alert.needs], [b.lease.unit_id, "audit", [], null, null]);
    assert.deepEqual(alert.runs, [
        { user: "owner", host, role: "audit", kind: null, backend: rb.backend, digest: rb.digest, samples: 400000, failures: 2, seconds: 97.3 },
    ]);
    const listed = await new Promise((resolve) => {
        execFile(process.execPath, [path.join(SERVER_DIR, "scripts", "admin.mjs"), "--server", srv.url, "alerts"], { env: { ...process.env, ADMIN_SECRET } }, (err, stdout, stderr) =>
            resolve({ code: err ? err.code : 0, stdout, stderr }),
        );
    });
    assert.equal(listed.code, 0, listed.stderr);
    assert.match(
        listed.stdout,
        new RegExp(`unit ${b.lease.unit_id} .*an audit found 2 failures in 400000 residues \\(97\\.3 s\\); owner host ${host.slice(0, 8)} ${rb.backend}, seed ${rb.digest.slice(0, 16)}`),
    );
    const resolved = await api.post("/api/admin/alerts/resolve", { id: alert.id, note: "reran the audit and the block; both clean" }, auth);
    assert.equal(resolved.status, 200, resolved.text);
    s = (await statsOf(id)).body;
    assert.deepEqual([s.audits, s.alerts_open], [{ passed: 1, failed: 1 }, 0], "the count stays when the alert is resolved");
});
