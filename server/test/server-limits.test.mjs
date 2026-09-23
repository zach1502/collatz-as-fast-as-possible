// npm test: the rate limits, the rows a fetch reads, and the admin API
// switch. helpers-server.mjs starts the Worker and holds the shared helpers.

import assert from "node:assert/strict";
import crypto from "node:crypto";
import { test } from "node:test";

import { CAMPAIGNS, COMMIT, RL_SECRET, SHORT_ADMIN_SECRET, TOKEN, cost, fetchFor, machines, off, skip, srv } from "./helpers-server.mjs";
import { client, newHost } from "./helpers.mjs";

test("rate limits: the binding and the D1 fallback both answer 429", { skip }, async () => {
    const viaBinding = client(srv.url, "10.9.0.1");
    const codes = [];
    for (let i = 0; i < 50; i++) {
        codes.push((await viaBinding.get("/api/ledger/head")).status);
    }
    assert.ok(codes.slice(0, 40).every((c) => c === 200), codes.join(","));
    assert.ok(codes.includes(429), "the per-IP binding limit (40 per 10 s) must trip");

    // The fallback counts per wall-clock minute, so the test waits out the last 10 s of a minute.
    while (Date.now() % 60000 > 50000) {
        await new Promise((r) => setTimeout(r, 500));
    }
    const viaD1 = client(srv.url, "10.9.0.2");
    const d1codes = [];
    for (let i = 0; i < 24; i++) {
        d1codes.push((await viaD1.get("/api/ledger/head", { "x-test-rate-limiter": "d1" })).status);
    }
    assert.ok(d1codes.slice(0, 20).every((c) => c === 200), d1codes.join(","));
    assert.deepEqual(d1codes.slice(20), [429, 429, 429, 429]);
});

test("rate limits: an IPv6 client is counted by its /64, an IPv4-mapped one as the IPv4 address", { skip }, async () => {
    while (Date.now() % 60000 > 50000) {
        await new Promise((r) => setTimeout(r, 500));
    }
    const get = (ip) => client(srv.url, ip).get("/api/ledger/head", { "x-test-rate-limiter": "d1" });
    const codes = [];
    for (let i = 0; i < 20; i++) {
        // Twenty different addresses in one /64, written in several forms.
        const ip = [`2001:db8:a:b::${i + 1}`, `2001:DB8:A:B:${i}:1:2:3`, `2001:0db8:000a:000b:0:0:0:${(i + 100).toString(16)}`][i % 3];
        codes.push((await get(ip)).status);
    }
    assert.ok(codes.every((c) => c === 200), codes.join(","));
    assert.equal((await get("2001:db8:a:b:ffff::9")).status, 429, "the 21st request from the /64 is over the 20 per minute");
    assert.equal((await get("2001:db8:a:c::1")).status, 200, "the next /64 has its own budget");
    const mac = (id) => `ip:${crypto.createHmac("sha256", RL_SECRET).update(id).digest("hex").slice(0, 32)}`;
    const keys = (await srv.query("SELECT key FROM rate WHERE key LIKE 'ip:%'")).map((r) => r.key);
    assert.ok(keys.includes(mac("2001:db8:a:b::/64")), "the counter is keyed on the prefix");
    assert.equal((await get("::ffff:10.9.0.77")).status, 200);
    const after = (await srv.query("SELECT key FROM rate WHERE key LIKE 'ip:%'")).map((r) => r.key);
    assert.ok(after.includes(mac("10.9.0.77")), "an IPv4-mapped address counts as the IPv4 address");
    assert.ok(!after.some((k) => k.includes("10.9.0.77")), "no counter holds an address");
});

test("the admin API is off while ADMIN_SECRET has fewer than 32 characters", { skip }, async () => {
    const api = client(srv.url, "10.1.0.21");
    const r = await api.get("/api/admin/alerts", { authorization: `Bearer ${SHORT_ADMIN_SECRET}` });
    assert.equal(r.status, 503, r.text);
    assert.match(r.body.error, /at least 32 characters/);
});

test("rate limits: ledger pages read from D1 have a budget per address; cached pages do not count", { skip }, async () => {
    // A page read from D1 costs 10 of the binding's 40 per 10 s, under a key of its own:
    // 12 reads within a second span at most two windows, so some are refused.
    const api = client(srv.url, "10.9.0.3");
    const answers = [];
    for (let i = 0; i < 12; i++) {
        answers.push(await api.get(i % 2 === 0 ? "/api/ledger" : "/api/ledger.csv"));
    }
    const codes = answers.map((r) => r.status);
    assert.deepEqual(codes.slice(0, 4), [200, 200, 200, 200], codes.join(","));
    const refused = answers.find((r) => r.status === 429);
    assert.ok(refused !== undefined, `some page reads are refused: ${codes.join(",")}`);
    assert.match(refused.body.error, /too many ledger pages/);
    assert.equal(refused.rowsRead, 0, "a refused page reads no row");
    assert.equal((await api.get("/api/ledger/head")).status, 200, "the head reads no page");
    const cached = client(srv.url, "10.9.0.4");
    const on = { "x-test-stats-cache-seconds": "30" };
    const hits = [];
    for (let i = 0; i < 12; i++) {
        hits.push((await cached.get("/api/ledger?from=1", on)).status);
    }
    assert.ok(hits.every((c) => c === 200), hits.join(","));
});

test("rate limits: in the D1 fallback a request counts against its address only after its token checked out", { skip }, async () => {
    const d1 = { "x-test-rate-limiter": "d1" };
    const api = client(srv.url, "10.9.0.6");
    const bad = await api.post("/api/fetch", { token: "x".repeat(40), host: newHost(), commit: COMMIT, campaign: CAMPAIGNS.basic }, d1);
    assert.equal(bad.status, 401, bad.text);
    cost.push({ what: "fetch, wrong token (D1 limiter)", read: bad.rowsRead, written: bad.rowsWritten });
    assert.deepEqual([bad.rowsRead, bad.rowsWritten], [0, 0], "a refused token costs no D1 row");
    const mac = `ip:${crypto.createHmac("sha256", RL_SECRET).update("10.9.0.6").digest("hex").slice(0, 32)}`;
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM rate WHERE key = '${mac}'`))[0].n, 0);
    const m = machines();
    const ok = await api.post("/api/fetch", { token: TOKEN, host: m.hosts[0], commit: COMMIT, campaign: CAMPAIGNS.basic }, d1);
    assert.equal(ok.status, 200, ok.text);
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM rate WHERE key = '${mac}'`))[0].n, 1, "an accepted token counts");
    await api.post("/api/release", { token: TOKEN, host: m.hosts[0], unit_id: ok.body.lease.unit_id, lease_id: ok.body.lease.lease_id });
});

test("fetch: the scan reads a bounded number of rows however many units ahead hold the owner's runs", { skip }, async () => {
    const api = client(srv.url, "10.1.0.14");
    const id = CAMPAIGNS.scan;
    const m = machines();
    // 300 open units, each holding a submitted run of the owner's and waiting for one more.
    await srv.sql(
        `WITH RECURSIVE i(x) AS (SELECT 0 UNION ALL SELECT x + 1 FROM i WHERE x < 299)
         INSERT INTO units (campaign_id, pos, bits, len, wanted, results, created_at) SELECT ${id}, x, 32, 1, 2, 1, 0 FROM i;
         INSERT INTO runs (unit_id, user, host, state, leased_at, expires_at, submitted_at, digest)
         SELECT id, 'owner', '${m.hosts[0]}', 'submitted', 0, 0, 0, 'd' FROM units WHERE campaign_id = ${id};
         UPDATE campaigns SET alloc_pos = 300 WHERE id = ${id};`,
    );
    const f = await fetchFor(api, m, id);
    assert.equal(f.status, 200, f.text);
    assert.equal(off(f.body.lease), 300, "every unit in the window holds a run of the owner's: a new one opens");
    assert.ok(f.rowsRead < 200, `fetch read ${f.rowsRead} rows`);
    cost.push({ what: "fetch (300 units ahead hold runs)", read: f.rowsRead, written: f.rowsWritten });
});
