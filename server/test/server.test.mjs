// npm test: the JS digest port and the fixtures, the worker token and the
// commit allow-list, a unit's result and its re-delivery, and the static page.
// helpers-server.mjs starts the Worker and holds the shared helpers.

import assert from "node:assert/strict";
import { test } from "node:test";

import { CAMPAIGNS, COMMIT, FIX0, FIX1, H0, TOKEN, at, cost, fetchFor, machines, off, releaseFor, resultFor, skip, srv, statsOf, submitFor, unitOf } from "./helpers-server.mjs";
import { client, digestTextJs, parseExact, redigest, sha256Hex } from "./helpers.mjs";

test("the JS digest port reproduces the binary's result_digest", { skip }, () => {
    for (const text of [FIX0, FIX1]) {
        const d = parseExact(text);
        assert.equal(sha256Hex(digestTextJs(d)), d.result.result_digest);
    }
});

test("fetch: the worker token and the commit allow-list", { skip }, async () => {
    const api = client(srv.url, "10.1.0.2");
    const m = machines();
    const noToken = await api.post("/api/fetch", { host: m.hosts[0], commit: COMMIT, campaign: CAMPAIGNS.basic });
    assert.equal(noToken.status, 401, noToken.text);
    const r = await api.post("/api/fetch", { token: TOKEN, host: m.hosts[0], commit: "deadbeef", campaign: CAMPAIGNS.basic });
    assert.equal(r.status, 403);
    assert.deepEqual(r.body.allowed_commits, [COMMIT]);
    assert.equal((await api.post("/api/fetch", { token: TOKEN, host: "xyz", commit: COMMIT })).status, 400);
});

test("fetch: lowest unit first, re-delivery, one result per unit, and an accepted unit is not handed back", { skip }, async () => {
    const api = client(srv.url, "10.1.0.3");
    const id = CAMPAIGNS.basic;
    const m = machines(2);
    const a = await fetchFor(api, m, id);
    assert.equal(a.status, 200, a.text);
    assert.equal(a.body.lease.h, H0.toString());
    assert.equal(a.body.lease.k, 32);
    assert.equal(a.body.lease.note, null);
    cost.push({ what: "fetch (opens a unit)", read: a.rowsRead, written: a.rowsWritten });
    const again = await fetchFor(api, m, id);
    assert.equal(again.body.lease.unit_id, a.body.lease.unit_id);
    assert.equal(again.body.redelivered, true);
    const b = await fetchFor(api, m, id, {}, 1);
    assert.equal(off(b.body.lease), 1, "another machine gets the next unit");

    const s1 = await submitFor(api, m, a.body.lease.unit_id, FIX0);
    assert.equal(s1.status, 200, s1.text);
    assert.deepEqual([s1.body.state, s1.body.verdict], ["accepted", "accepted"]);
    cost.push({ what: "submit (accepts the unit)", read: s1.rowsRead, written: s1.rowsWritten });
    const dup = await submitFor(api, m, a.body.lease.unit_id, FIX0);
    assert.equal(dup.body.duplicate, true);
    const other = await submitFor(api, m, a.body.lease.unit_id, resultFor(0, (d) => (d.result.blocks_total = 1)));
    assert.equal(other.status, 409, "one result per unit");

    const again2 = await fetchFor(api, m, id, { prefer_unit: a.body.lease.unit_id });
    assert.notEqual(again2.body.lease.unit_id, a.body.lease.unit_id, "an accepted unit is not handed back");
    const st = await statsOf(api, id);
    assert.deepEqual([st.totals.blocks, st.frontier.value], [1, at(1)]);
    const u = await unitOf(a.body.lease.unit_id);
    assert.deepEqual([u.state, u.checks, u.digest], ["accepted", 1, parseExact(FIX0).result.result_digest]);
    const [stored] = await srv.query(`SELECT result_json FROM unit_results WHERE unit_id = ${a.body.lease.unit_id}`);
    const pow = /"pow_checksum": (\d+)/.exec(FIX0)[1];
    assert.ok(stored.result_json.includes(`"pow_checksum":${pow}`), "stored u64 counters stay exact");
    await releaseFor(api, m, again2.body.lease.unit_id);
    await releaseFor(api, m, b.body.lease.unit_id, 1);
});

test("static page and unknown endpoints", { skip }, async () => {
    const api = client(srv.url, "10.1.0.11");
    const page = await api.get("/");
    assert.equal(page.status, 200);
    assert.match(page.text, /<html/i);
    for (const r of [await api.get("/api/nothing"), await api.post("/api/nothing", {}), await api.get("/api/fetch/1")]) {
        assert.equal(r.status, 404, r.text);
        assert.match(r.body.error, /^no endpoint (GET|POST) \/api\//);
    }
    const get = await api.get("/api/fetch");
    assert.equal(get.status, 405, get.text);
    assert.match(get.body.error, /takes POST/);
});

test("tampered fixtures: an edited field without a new digest is caught", { skip }, () => {
    const d = parseExact(FIX0);
    const before = d.result.result_digest;
    redigest(d);
    assert.equal(d.result.result_digest, before);
    d.result.overflow_fallbacks = 1;
    redigest(d);
    assert.notEqual(d.result.result_digest, before);
});
