// npm test: the ledger entries of an accepted block and of an agreeing
// re-check, the ledger in /api/stats, the page's ledger line, and the refusals
// that keep entries unchanged, which act on the entries the earlier tests add.
// The Worker and the helpers are in test/helpers-ledger.mjs.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { test } from "node:test";

import { BARINA_L0 } from "../scripts/campaign.mjs";
import {
    ADMIN,
    COMMIT,
    GENESIS,
    OWNER,
    SCHEMA,
    W,
    assertChain,
    entriesOf,
    fetchAs,
    frontierOf,
    newApi,
    nowS,
    owner,
    readLedger,
    resultFor,
    runOne,
    serveLedgerOwner,
    skip,
    submitAs,
    withSeconds,
} from "./helpers-ledger.mjs";
import { SERVER_DIR, newHost, sha256Hex } from "./helpers.mjs";

serveLedgerOwner();

test("an accepted block gets a 'block' entry with its result's fields, hashed over its canonical line", { skip }, async () => {
    const api = newApi(owner);
    const host = newHost();
    const before = (await api.get("/api/ledger/head")).body;
    const t0 = nowS();
    const { lease, doc } = await runOne(api, host, OWNER.accept, { extra: { bits: 33 }, seconds: "12.500000" });
    // The document's backend label is free text; the entry keeps params.gpu.
    const gpu = await runOne(api, host, OWNER.accept, {
        extra: { bits: 32 },
        edit: (d) => {
            d.params.gpu = true;
            d.backend = "OpenCL | GPU 0";
        },
    });
    const t1 = nowS();
    const [e, g] = await entriesOf(owner, OWNER.accept);
    const label = host.slice(0, 8);
    const line =
        `collatz-ledger/2|${before.seq + 1}|${OWNER.accept}|${e.at}|block|${lease.l}|33|${lease.end}|${doc.result.result_digest}|` +
        `${doc.result.max_excursion}|${doc.result.max_excursion_n}|12.500000|${label}|cpu|${COMMIT}|${SCHEMA}|owner|1|${before.hash}`;
    assert.deepEqual(e, {
        format: "collatz-ledger/2",
        seq: before.seq + 1,
        campaign: OWNER.accept,
        at: e.at,
        kind: "block",
        start: lease.l,
        bits: 33,
        end: lease.end,
        result: doc.result.result_digest,
        peak: doc.result.max_excursion,
        peak_n: doc.result.max_excursion_n,
        seconds: "12.500000",
        machine: label,
        backend: "cpu",
        version: COMMIT,
        schema: SCHEMA,
        who: "owner",
        votes: 1,
        prev: before.hash,
        hash: sha256Hex(line),
    });
    assert.ok(e.at >= t0 && e.at <= t1, `at ${e.at} is the time of the submit`);
    assert.equal(BigInt(e.end) - BigInt(e.start), 1n << 33n);
    const [u] = await owner.query(`SELECT accepted_at, digest FROM units WHERE id = ${lease.unit_id}`);
    assert.deepEqual([u.accepted_at, u.digest], [e.at, e.result], "the unit's acceptance time and digest");
    assert.deepEqual(
        [g.seq, g.prev, g.bits, g.start, g.backend, g.seconds, g.machine, g.result],
        [e.seq + 1, e.hash, 32, gpu.lease.l, "gpu", "0.142997", label, gpu.doc.result.result_digest],
    );
});

test("a re-check that agrees adds a 'recheck' entry of the re-check's own run, and the block keeps counting", { skip }, async () => {
    const id = OWNER.recheck; // recheck_rate 1, RECHECK_EVERY 0: the next fetch is the re-check
    const api = newApi(owner);
    const [h1, h2] = [newHost(), newHost()];
    const u = await runOne(api, h1, id);
    const f = await fetchAs(api, h2, id);
    assert.equal(f.body.lease.unit_id, u.lease.unit_id, "the other machine gets the re-check");
    const s = await submitAs(api, h2, u.lease.unit_id, withSeconds(resultFor(f.body.lease), "3.000000"));
    assert.equal(s.status, 200, s.text);
    assert.equal((await owner.query(`SELECT recheck FROM units WHERE id = ${u.lease.unit_id}`))[0].recheck, "passed");
    const [b, r] = await entriesOf(owner, id);
    assert.deepEqual([b.kind, r.kind], ["block", "recheck"]);
    assert.deepEqual([r.start, r.bits, r.end, r.result, r.peak], [b.start, b.bits, b.end, b.result, b.peak], "the same block and digest");
    assert.deepEqual([b.machine, r.machine, r.seconds, r.backend, r.version], [h1.slice(0, 8), h2.slice(0, 8), "3.000000", "cpu", COMMIT]);
    assert.equal(await frontierOf(id), (BARINA_L0 + W).toString());
});

test("/api/stats gives the ledger's entry count and its head's hash", { skip }, async () => {
    const api = newApi(owner);
    const head = (await api.get("/api/ledger/head")).body;
    const s = (await api.get(`/api/stats?campaign=${OWNER.accept}`)).body;
    assert.deepEqual(s.ledger, { entries: head.seq, head: head.hash.slice(0, 16), head_full: head.hash });
});

test("an ADMIN_SECRET stored with a trailing newline opens the admin API, as WORKER_TOKEN does", { skip }, async () => {
    const r = await newApi(owner).get("/api/admin/alerts", ADMIN);
    assert.equal(r.status, 200, r.text);
    assert.ok(Array.isArray(r.body.alerts));
    const nl = await newApi(owner).get("/api/admin/alerts", { authorization: `${ADMIN.authorization}x` });
    assert.equal(nl.status, 401, "another secret is still refused");
});

test("the page has the ledger line; no code changes or deletes an entry, and the database refuses it", { skip }, async () => {
    const api = newApi(owner);
    const page = (await api.get("/")).text;
    assert.match(
        page,
        /<footer>\n<p>Data: <a href="\/api\/stats">\/api\/stats<\/a><\/p>\n<p id="ledger" hidden>Ledger: <span id="ledger-entries"><\/span><span id="ledger-latest"><\/span> \(<a href="\/api\/ledger\.csv">CSV<\/a>, <a href="\/api\/ledger">JSON<\/a>\)<\/p>\n<\/footer>/,
    );
    const js = (await api.get("/app.js")).text;
    for (const name of ["ledger", "ledger-entries", "ledger-latest"]) {
        assert.ok(js.includes(`$("${name}")`), name);
    }
    assert.match(js, /\.head_full\b/);

    const files = ["src", "scripts"].flatMap((dir) =>
        fs.readdirSync(path.join(SERVER_DIR, dir)).map((f) => `${dir}/${f}`).filter((f) => fs.statSync(path.join(SERVER_DIR, f)).isFile()),
    );
    const text = (f) => fs.readFileSync(path.join(SERVER_DIR, f), "utf8");
    const changes = /\b(?:UPDATE(?:\s+OR\s+\w+)?|DELETE\s+FROM|REPLACE\s+INTO|INSERT\s+OR\s+\w+\s+INTO|DROP\s+TABLE(?:\s+IF\s+EXISTS)?|ALTER\s+TABLE)\s+"?ledger"?(?!\w)/i;
    for (const f of files) {
        assert.doesNotMatch(text(f), changes, f);
    }
    assert.deepEqual(
        files.filter((f) => /INSERT\s+INTO\s+"?ledger"?(?!\w)/i.test(text(f))),
        ["src/ledger.ts"],
        "entries are appended by the Worker only",
    );

    const { head } = await readLedger(owner);
    const cols = 'seq, campaign_id, at, kind, start, bits, "end", result, peak, peak_n, seconds, machine, backend, version, schema, prev, hash';
    const entry = (seq, prev) => `INSERT INTO ledger (${cols}) VALUES (${seq}, 1, 0, 'block', '0', 32, '1', '', '', '', '', '', '', '', '', '${prev}', 'x')`;
    await assert.rejects(owner.query("UPDATE ledger SET kind = 'withdrawn' WHERE seq = 1"), /ledger entries are never changed/);
    await assert.rejects(owner.query("DELETE FROM ledger WHERE seq = 1"), /ledger entries are never deleted/);
    await assert.rejects(owner.query(entry(head.seq + 2, head.hash)), /a ledger entry must follow the head/);
    await assert.rejects(owner.query(entry(head.seq + 1, GENESIS)), /a ledger entry must follow the head/);
    await assert.rejects(owner.query(`UPDATE ledger_head SET seq = 0, hash = '${GENESIS}' WHERE id = 1`), /the ledger head must be its last entry/);
    await assert.rejects(owner.query("DELETE FROM ledger_head"), /the ledger head is never deleted/);
    assertChain(await readLedger(owner));
});
