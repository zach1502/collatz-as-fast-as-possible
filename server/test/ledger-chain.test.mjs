// npm test: the chain over many and racing submits, a withdrawn block, the
// ledger endpoints, and verify-ledger. The endpoints and verify-ledger read
// the entries the earlier tests of this file add. The Worker and the helpers
// are in test/helpers-ledger.mjs.

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";

import { BARINA_L0 } from "../scripts/campaign.mjs";
import {
    ADMIN,
    COLUMNS,
    COMMIT,
    OWNER,
    SCHEMA,
    W,
    entriesOf,
    fetchAs,
    frontierOf,
    isoOf,
    lineOf,
    measure,
    newApi,
    nowS,
    owner,
    readLedger,
    resultFields,
    resultFor,
    runOne,
    runScript,
    serveLedgerOwner,
    skip,
    submitAs,
    wrong,
} from "./helpers-ledger.mjs";
import { newHost, sha256Hex } from "./helpers.mjs";

serveLedgerOwner();

test("the chain stays whole over many submits, also when two machines submit at once or one loses the race", { skip }, async () => {
    const id = OWNER.chain;
    const machines = [
        { api: newApi(owner), host: newHost() },
        { api: newApi(owner), host: newHost() },
    ];
    const rounds = 5;
    for (let i = 0; i < rounds; i++) {
        const leases = [];
        for (const m of machines) {
            leases.push((await fetchAs(m.api, m.host, id)).body.lease);
        }
        const answers = await Promise.all(machines.map((m, j) => submitAs(m.api, m.host, leases[j].unit_id, resultFor(leases[j]))));
        for (const s of answers) {
            assert.equal(s.status, 200, s.text);
            assert.equal(s.body.state, "accepted");
        }
    }
    // The first insert finds another entry appended first: the submit reads again and retries.
    const [m] = machines;
    const f = await fetchAs(m.api, m.host, id);
    const raced = await submitAs(m.api, m.host, f.body.lease.unit_id, resultFor(f.body.lease), { "x-test-race": "3" });
    assert.equal(raced.status, 200, raced.text);
    assert.equal(raced.body.state, "accepted");

    const mine = await entriesOf(owner, id);
    assert.equal(mine.length, 2 * rounds + 1);
    assert.ok(mine.every((e) => e.kind === "block"));
    assert.equal(new Set(mine.map((e) => e.start)).size, mine.length, "one entry per block");
    assert.deepEqual(new Set(mine.map((e) => e.machine)), new Set(machines.map((x) => x.host.slice(0, 8))));
    const [n] = await owner.query(`SELECT COUNT(*) AS n FROM units WHERE campaign_id = ${id} AND state = 'accepted'`);
    assert.equal(n.n, mine.length, "every accepted block has its entry");
});

test("a disagreeing re-check withdraws the block and the frontier is as before it; a decision adds it again, a requeue adds nothing", { skip }, async () => {
    const id = OWNER.conflict; // recheck_rate 1: the owner's next fetch on the machine is the re-check
    const api = newApi(owner);
    const host = newHost();
    const before = await frontierOf(id);
    const u = await runOne(api, host, id, { seconds: "7.250000" });
    assert.equal(await frontierOf(id), (BARINA_L0 + W).toString());
    const f = await fetchAs(api, host, id);
    assert.equal(f.body.lease.unit_id, u.lease.unit_id);
    assert.equal((await submitAs(api, host, u.lease.unit_id, resultFor(f.body.lease, wrong))).status, 200);
    assert.equal((await owner.query(`SELECT state FROM units WHERE id = ${u.lease.unit_id}`))[0].state, "conflict");
    assert.equal(await frontierOf(id), before, "the frontier is back where it was before the block");
    let mine = await entriesOf(owner, id);
    assert.deepEqual(mine.map((e) => e.kind), ["block", "withdrawn"]);
    const [b, w] = mine;
    assert.deepEqual(
        { ...w, seq: 0, at: 0, prev: "", hash: "" },
        { ...b, seq: 0, at: 0, prev: "", hash: "", kind: "withdrawn", peak: "", peak_n: "", seconds: "", machine: "", backend: "", version: "", who: "", votes: 0 },
        "the block and the digest it was accepted with",
    );

    const digest = u.doc.result.result_digest;
    const dec = await api.post("/api/admin/decide", { unit_id: u.lease.unit_id, digest, note: "the first result stands" }, ADMIN);
    assert.equal(dec.status, 200, dec.text);
    mine = await entriesOf(owner, id);
    assert.deepEqual(mine.map((e) => e.kind), ["block", "withdrawn", "block"]);
    assert.deepEqual(resultFields(mine[2]), resultFields(b), "the run the unit is accepted with again");
    assert.equal(await frontierOf(id), (BARINA_L0 + W).toString());

    // A block that left the accepted state already has its withdrawn entry; starting it over adds no entry.
    const u2 = await runOne(api, host, id);
    const f2 = await fetchAs(api, host, id);
    assert.equal(f2.body.lease.unit_id, u2.lease.unit_id);
    assert.equal((await submitAs(api, host, u2.lease.unit_id, resultFor(f2.body.lease, wrong))).status, 200);
    const head = (await newApi(owner).get("/api/ledger/head")).body;
    const rq = await api.post("/api/admin/decide", { unit_id: u2.lease.unit_id, requeue: true, note: "run it again" }, ADMIN);
    assert.deepEqual([rq.status, rq.body.state], [200, "open"], rq.text);
    assert.deepEqual((await newApi(owner).get("/api/ledger/head")).body, head);
    mine = await entriesOf(owner, id);
    assert.deepEqual(mine.map((e) => e.kind), ["block", "withdrawn", "block", "block", "withdrawn"]);
});

test("the endpoints: JSON pages by seq, the CSV, the head, the cache time, and reads through the primary key", { skip }, async () => {
    // A fresh address per request: every page read from D1 counts against the address's page budget.
    const get = (p, headers) => newApi(owner).get(p, headers);
    const { head, entries } = await readLedger(owner);
    assert.ok(entries.length >= 10, `${entries.length} entries`);
    const all = await get("/api/ledger");
    assert.equal(all.status, 200, all.text);
    assert.match(all.headers.get("content-type"), /^application\/json/);
    assert.deepEqual(all.body, entries);
    assert.ok(all.rowsRead <= entries.length, `the page read ${all.rowsRead} rows`);
    // Pages hold 1000 entries from seq 1, 1001, ...: from is rounded down to the start of its page.
    for (const q of ["from=2", "from=0", `from=${head.seq}`, "from=1000", "limit=1000", "from=5&limit=1000"]) {
        assert.deepEqual((await get(`/api/ledger?${q}`)).body, entries, q);
    }
    assert.deepEqual((await get("/api/ledger?from=1001")).body, [], "the next page is empty");
    for (const q of ["limit=0", "limit=3", "limit=1001", "limit=-1", "limit=x", "from=1.5", "from=-1"]) {
        const r = await get(`/api/ledger?${q}`);
        assert.equal(r.status, 400, `${q}: ${r.text}`);
    }

    const csv = await get("/api/ledger.csv?from=2");
    assert.equal(csv.status, 200, csv.text);
    assert.match(csv.headers.get("content-type"), /^text\/csv/);
    assert.equal(csv.headers.get("content-disposition"), 'attachment; filename="ledger.csv"');
    const rows = csv.text.split("\r\n");
    assert.equal(rows.pop(), "", "every row ends with CRLF");
    assert.equal(rows[0], COLUMNS.join(","));
    assert.deepEqual(rows.slice(1).map((l) => l.split(",")), entries.map((e) => COLUMNS.map((k) => String(e[k]))));
    const whole = await get("/api/ledger.csv");
    assert.equal(whole.text.split("\r\n").length, entries.length + 2, "a header row and one row per entry");

    const h = await get("/api/ledger/head");
    assert.deepEqual(h.body, { seq: entries.length, hash: entries[entries.length - 1].hash, at: entries[entries.length - 1].at });
    assert.equal(h.rowsRead, 1);
    assert.equal(h.headers.get("cache-control"), "no-store", "the test environment does not cache");
    const on = { "x-test-stats-cache-seconds": "30" };
    for (const [p, same] of [
        ["/api/ledger/head", "/api/ledger/head"],
        ["/api/ledger?from=3", "/api/ledger?from=7&limit=1000"],
        ["/api/ledger.csv?from=3", "/api/ledger.csv"],
    ]) {
        const r1 = await get(p, on);
        assert.deepEqual([r1.headers.get("x-ledger-cache"), r1.headers.get("cache-control")], ["miss", "public, max-age=30"], p);
        measure(`ledger ${p} (cache miss)`, r1);
        for (const q of [p, same]) {
            const r2 = await get(q, on);
            assert.deepEqual([r2.headers.get("x-ledger-cache"), r2.rowsRead, r2.text], ["isolate", 0, r1.text], `${q} after ${p}`);
            measure(`ledger ${q} (same page, cached)`, r2);
        }
    }
    const plan = await owner.query("EXPLAIN QUERY PLAN SELECT seq FROM ledger WHERE seq >= 2 ORDER BY seq LIMIT 3");
    assert.match(plan.map((p) => p.detail).join("\n"), /SEARCH ledger USING INTEGER PRIMARY KEY/);
});

/**
 * Appends a collatz-ledger/1 'block' entry for slot `pos` of the v1 campaign
 * through SQL. A /1 entry has an empty who and votes 0 (migration 0005 adds
 * both columns), and its hash covers the /1 line.
 */
async function appendV1Block(pos) {
    const head = (await newApi(owner).get("/api/ledger/head")).body;
    const start = BARINA_L0 + pos * W;
    const e = {
        format: "collatz-ledger/1",
        seq: head.seq + 1,
        campaign: OWNER.v1,
        at: nowS(),
        kind: "block",
        start: start.toString(),
        bits: 32,
        end: (start + W).toString(),
        result: "d".repeat(64),
        peak: "",
        peak_n: "",
        seconds: "",
        machine: "",
        backend: "cpu",
        version: COMMIT,
        schema: SCHEMA,
        who: "",
        votes: 0,
        prev: head.hash,
    };
    e.hash = sha256Hex(lineOf(e));
    const column = (k) => (k === "campaign" ? "campaign_id" : k === "end" ? '"end"' : k);
    const literal = (v) => (typeof v === "number" ? String(v) : `'${v}'`);
    await owner.query(`INSERT INTO ledger (${COLUMNS.map(column).join(", ")}) VALUES (${COLUMNS.map((k) => literal(e[k])).join(", ")})`);
}

test("verify-ledger: the ledger verifies from the server, as JSON and as CSV; a tampered copy fails at the first bad seq", { skip }, async () => {
    // Slots 0 and 2 of the v1 campaign, so the ledger also holds collatz-ledger/1 entries.
    for (const pos of [0n, 2n]) {
        await appendV1Block(pos);
    }
    const { head, entries } = await readLedger(owner);
    const ok = await runScript("verify-ledger.mjs", [owner.url]);
    assert.equal(ok.code, 0, ok.stdout + ok.stderr);
    assert.match(ok.stdout, new RegExp(`^entries: ${head.seq}\nhead: ${head.seq} ${head.hash} at ${isoOf(head.at)}\n`, "m"));
    assert.match(ok.stdout, /\nledger OK\n$/);
    // The chain campaign: 11 contiguous blocks from L0, up to the server's own frontier.
    const chain = await frontierOf(OWNER.chain);
    assert.match(ok.stdout, new RegExp(`^campaign ${OWNER.chain}: blocks 11, numbers ${11n * W}, frontier ${chain} \\(contiguous from ${BARINA_L0}\\)$`, "m"));
    // The v1 campaign: both collatz-ledger/1 blocks count, and the gap at slot 1 ends the frontier.
    assert.match(ok.stdout, new RegExp(`^campaign ${OWNER.v1}: blocks 2, numbers ${2n * W}, frontier ${BARINA_L0 + W} \\(`, "m"));
    // The conflict campaign: block, withdrawn, block, block, withdrawn leaves one block.
    assert.match(ok.stdout, new RegExp(`^campaign ${OWNER.conflict}: blocks 1, numbers ${W}, `, "m"));

    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "collatz-ledger-verify-"));
    try {
        const save = (name, text) => {
            const f = path.join(dir, name);
            fs.writeFileSync(f, text);
            return f;
        };
        const csv = (await newApi(owner).get("/api/ledger.csv")).text;
        // CSV pages appended one after another repeat the header (here the first page and the empty second one).
        const csvPages = csv + (await newApi(owner).get("/api/ledger.csv?from=1001")).text;
        for (const f of [save("ledger.json", JSON.stringify(entries)), save("ledger.csv", csv), save("pages.csv", csvPages)]) {
            const r = await runScript("verify-ledger.mjs", [f]);
            assert.equal(r.code, 0, `${f}: ${r.stdout}${r.stderr}`);
            assert.match(r.stdout, new RegExp(`^head: ${head.seq} ${head.hash} at `, "m"));
        }

        const k = entries.findIndex((e) => e.kind === "block" && e.peak !== "" && e.seq > 1 && e.seq < entries.length);
        const seq = k + 1;
        const tampered = {
            "a changed peak": entries.map((e, i) => (i === k ? { ...e, peak: (BigInt(e.peak) + 1n).toString() } : e)),
            "a removed row": entries.filter((_, i) => i !== k),
            "two rows swapped": entries.map((e, i) => (i === k ? entries[k + 1] : i === k + 1 ? entries[k] : e)),
        };
        for (const [what, copy] of Object.entries(tampered)) {
            const r = await runScript("verify-ledger.mjs", [save("copy.json", JSON.stringify(copy))]);
            assert.equal(r.code, 1, `${what}: ${r.stdout}${r.stderr}`);
            assert.match(r.stdout, new RegExp(`^FAIL at seq ${seq}: `), what);
        }
        const lines = csv.split("\r\n");
        const cells = lines[seq].split(",");
        cells[COLUMNS.indexOf("peak")] = (BigInt(cells[COLUMNS.indexOf("peak")]) + 1n).toString();
        lines[seq] = cells.join(",");
        const r = await runScript("verify-ledger.mjs", [save("copy.csv", lines.join("\r\n"))]);
        assert.deepEqual([r.code, r.stdout], [1, `FAIL at seq ${seq}: hash is not the SHA-256 of the entry's canonical line\n`]);

        // An anchor of the head, and one of an earlier entry, hold; one with another hash fails at its seq.
        const headAnchor = `collatz-ledger head ${head.seq} ${head.hash} at ${isoOf(head.at)}\n`;
        const anchors = save("anchors.txt", `collatz-ledger head 3 ${entries[2].hash} at ${isoOf(entries[2].at)}\n${headAnchor}`);
        const a = await runScript("verify-ledger.mjs", [owner.url, "--anchors", anchors]);
        assert.equal(a.code, 0, a.stdout + a.stderr);
        assert.match(a.stdout, /^anchors: 2 checked$/m);
        const wrongAnchor = save("wrong.txt", `collatz-ledger head 3 ${entries[3].hash} at ${isoOf(entries[3].at)}\n`);
        const b = await runScript("verify-ledger.mjs", [save("ledger.json", JSON.stringify(entries)), "--anchors", wrongAnchor]);
        assert.equal(b.code, 1, b.stdout + b.stderr);
        assert.match(b.stdout, /^FAIL at seq 3: is anchored with hash /);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
