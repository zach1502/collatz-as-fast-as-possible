// npm test: campaign.mjs's defaults and refusals, acceptance on one result,
// the frontier, and machine-chosen block sizes. The Worker and the helpers are
// in test/helpers-campaign.mjs.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import { BARINA_L0, campaignSql } from "../scripts/campaign.mjs";
import {
    CAMPAIGNS,
    campaignRow,
    fetchFor,
    frontierOf,
    machines,
    measure,
    pow,
    releaseFor,
    resultFor,
    runOne,
    serveCampaigns,
    skip,
    span,
    srv,
    statsOf,
    submitFor,
    unitRow,
} from "./helpers-campaign.mjs";

serveCampaigns();

test("campaign.mjs: defaults and refusals", () => {
    const text = campaignSql({ name: "first", commits: ["abc"] });
    assert.match(text, /min_bits, max_bits, a, b/);
    assert.match(text, /'2392312122059207475200', 44, 54, 12, 32, 'table16'/);
    assert.match(text, /'collatz-result-digest\/2', 1, 0, 43200/);
    const refused = (o, re) => assert.throws(() => campaignSql({ name: "x", commits: ["abc"], ...o }), re);
    refused({ L0: (BARINA_L0 + pow(50)).toString() }, /multiple of 2\^54/);
    refused({ recheckRate: 1.5 }, /--recheck-rate/);
    refused({ recheckRate: -0.1 }, /--recheck-rate/);
    refused({ minBits: 31 }, /32 <= --min-bits/);
    refused({ minBits: 50, maxBits: 46 }, /--min-bits <= --max-bits/);
    refused({ B: 33 }, /--B must be 2\.\.32/);
    refused({ A: 15 }, /--A must be 1\.\.14/);
    refused({ minBits: 40, maxBits: 44, A: 40 }, /--A must be/);
    refused({ end: BARINA_L0.toString() }, /--end must be above --L0/);
    assert.match(campaignSql({ name: "y", commits: ["abc"], end: (BARINA_L0 + pow(60)).toString() }), /, 65536, /);
});

test("campaign.mjs: a -dirty, -nogit or unknown commit is refused unless --allow-dirty is given", () => {
    const script = fileURLToPath(new URL("../scripts/campaign.mjs", import.meta.url));
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "campaign-mjs-"));
    const out = path.join(dir, "campaign.sql");
    const run = (...args) => {
        fs.rmSync(out, { force: true });
        const r = spawnSync(process.execPath, [script, "--name", "x", "--out", out, ...args], { encoding: "utf8" });
        return { status: r.status, stderr: r.stderr, sql: fs.existsSync(out) ? fs.readFileSync(out, "utf8") : null };
    };
    try {
        const sha = "9b87fe8d9329610d559315e30908d0d4f00e1308";
        for (const commit of [`${sha}-dirty`, `${sha}-nogit`, "unknown", `${sha},${sha}-dirty`]) {
            const r = run("--commit", commit);
            assert.equal(r.status, 2, `--commit ${commit} is refused`);
            assert.match(r.stderr, /does not say which code ran.*--allow-dirty/);
            assert.equal(r.sql, null, `--commit ${commit} writes no SQL`);
        }
        const clean = run("--commit", sha);
        assert.equal(clean.status, 0, clean.stderr);
        assert.ok(clean.sql.includes(`'["${sha}"]'`), clean.sql);
        const allowed = run("--commit", `${sha}-dirty,${sha}-nogit`, "--allow-dirty");
        assert.equal(allowed.status, 0, allowed.stderr);
        assert.ok(allowed.sql.includes(`'["${sha}-dirty","${sha}-nogit"]'`), allowed.sql);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("quorum 1: one result accepts a unit and moves the frontier", { skip }, async () => {
    const id = CAMPAIGNS.q1;
    const m = machines(2);
    const a = await runOne(m, id, { bits: 33 });
    measure("fetch (opens a block)", a.fetchRes);
    measure("submit (accepts)", a.submitRes);
    assert.equal(a.lease.k, 33);
    assert.equal(a.lease.h, (BARINA_L0 / pow(33)).toString());
    assert.equal(a.lease.note, null);
    assert.deepEqual([a.submit.state, a.submit.verdict], ["accepted", "accepted"]);
    const c = await campaignRow(id);
    assert.deepEqual([c.quorum, c.min_bits, c.max_bits, c.units_accepted, c.units_twice], [1, 32, 34, 1, 0]);
    let s = await statsOf(id);
    assert.deepEqual([s.totals.blocks, s.totals.numbers_verified], [1, pow(33).toString()]);
    assert.equal(s.frontier.value, (BARINA_L0 + pow(33)).toString());
    const b = await fetchFor(m, id, { bits: 33 }, 1);
    assert.notEqual(b.body.lease.unit_id, a.lease.unit_id, "an accepted unit takes no second regular result");
    assert.equal(b.body.lease.h, (BARINA_L0 / pow(33) + 1n).toString());
    await submitFor(m, b.body.lease.unit_id, resultFor(b.body.lease), 1);
    s = await statsOf(id);
    assert.deepEqual([s.totals.blocks, s.frontier.value], [2, (BARINA_L0 + 2n * pow(33)).toString()]);
    const u = await unitRow(a.lease.unit_id);
    assert.deepEqual([u.state, u.checks, u.bits, u.recheck], ["accepted", 1, 33, null]);
    // Sizes outside the campaign's range are brought into it, and the lease says so.
    const big = await fetchFor(m, id, { bits: 40 });
    assert.equal(big.body.lease.k, 34);
    assert.match(big.body.lease.note, /blocks of 2\^32 to 2\^34 numbers/);
    await releaseFor(m, big.body.lease.unit_id);
});

test("block sizes: fetches of 2^50 and 2^46 get aligned blocks that do not overlap", { skip }, async () => {
    const id = CAMPAIGNS.sizes;
    const sizes = [50, 46, 48, 47, 46];
    // One machine per lease: a machine's live lease is handed out again by a fetch.
    const m = machines(sizes.length);
    const leases = [];
    for (const [i, bits] of sizes.entries()) {
        const f = await fetchFor(m, id, { bits }, i);
        assert.equal(f.status, 200, f.text);
        leases.push(f.body.lease);
    }
    assert.deepEqual(leases.map((l) => l.k), sizes);
    const slot = pow(46);
    // 2^50 at the start, 2^46 right after it, 2^48 aligned up past the padding,
    // which the 2^47 and 2^46 fetches then fill lowest first.
    assert.deepEqual(
        leases.map((l) => Number((BigInt(l.l) - BARINA_L0) / slot)),
        [0, 16, 20, 18, 17],
    );
    for (const l of leases) {
        const [s, e] = span(l);
        assert.equal(s % pow(l.k), 0n, `block ${l.h} of 2^${l.k} starts at a multiple of its size`);
        assert.equal(e - s, pow(l.k));
        assert.equal(BigInt(l.h) * pow(l.k), s);
    }
    for (let i = 0; i < leases.length; i++) {
        for (let j = i + 1; j < leases.length; j++) {
            const [a0, a1] = span(leases[i]);
            const [b0, b1] = span(leases[j]);
            assert.ok(a1 <= b0 || b1 <= a0, `leases ${i} and ${j} do not overlap`);
        }
    }
});

test("block sizes: with no room for the asked size near the frontier, the largest smaller free block, and the lease says so", { skip }, async () => {
    const id = CAMPAIGNS.fallback; // 2^46..2^50: the lookahead window is 2 blocks of 2^50 above the frontier
    const slot = pow(46);
    const m = machines(7);
    let next = 0;
    const at = async (bits) => {
        const f = await fetchFor(m, id, { bits }, next++);
        assert.equal(f.status, 200, f.text);
        return f.body.lease;
    };
    const offset = (l) => Number((BigInt(l.l) - BARINA_L0) / slot);
    const a = await at(50);
    const b = await at(46);
    assert.deepEqual([offset(a), offset(b)], [0, 16]);
    const c = await at(50);
    assert.deepEqual([c.k, offset(c)], [49, 24], "the next 2^50 would end past the window");
    assert.match(c.note, /a free block of 2\^49 numbers near the frontier is smaller than the 2\^50 asked/);
    const d = await at(50);
    assert.deepEqual([d.k, offset(d)], [48, 20], "the largest free piece in the window");
    const e = await at(50);
    const f = await at(50);
    assert.deepEqual([[e.k, offset(e)], [f.k, offset(f)]], [[47, 18], [46, 17]]);
    const g = await at(50);
    assert.deepEqual([g.k, offset(g), g.note], [50, 32, null], "with the window full, the asked size above it");
});

test("frontier: the end of the contiguous accepted prefix over blocks of mixed sizes and gaps", { skip }, async () => {
    const id = CAMPAIGNS.gaps; // slots of 2^32
    const m = machines(4);
    const fetchBits = async (bits, hostIdx) => (await fetchFor(m, id, { bits }, hostIdx)).body.lease;
    const A = await fetchBits(34, 0); // slots [0, 4)
    const B = await fetchBits(32, 1); // [4, 5)
    const C = await fetchBits(33, 2); // [6, 8); [5, 6) stays free
    const at = (slots) => (BARINA_L0 + BigInt(slots) * pow(32)).toString();
    assert.deepEqual([A.l, B.l, C.l], [at(0), at(4), at(6)]);
    assert.equal((await submitFor(m, C.unit_id, resultFor(C), 2)).body.state, "accepted");
    assert.equal(await frontierOf(id), at(0));
    assert.equal((await submitFor(m, B.unit_id, resultFor(B), 1)).body.state, "accepted");
    assert.equal(await frontierOf(id), at(0), "the first block is not accepted yet");
    await submitFor(m, A.unit_id, resultFor(A), 0);
    assert.equal(await frontierOf(id), at(5), "A and B; the free slot 5 stops it");
    const D = await fetchBits(32, 3);
    assert.equal(D.l, at(5), "the lowest free block of the asked size");
    await submitFor(m, D.unit_id, resultFor(D), 3);
    const s = await statsOf(id);
    assert.equal(s.frontier.value, at(8), "through C");
    assert.equal(s.totals.numbers_verified, (8n * pow(32)).toString());
    const units = await srv.query(`SELECT bits, state FROM units WHERE campaign_id = ${id} ORDER BY pos`);
    assert.deepEqual(units.map((u) => [u.bits, u.state]), [[34, "accepted"], [32, "accepted"], [32, "accepted"], [33, "accepted"]]);
});

test("frontier: a free block at the frontier is leased first, even when smaller than the asked size", { skip }, async () => {
    const id = CAMPAIGNS.hole; // slots of 2^32, blocks up to 2^36
    const m = machines(4);
    const fetchBits = async (bits, hostIdx) => (await fetchFor(m, id, { bits }, hostIdx)).body.lease;
    const at = (slots) => (BARINA_L0 + BigInt(slots) * pow(32)).toString();
    const A = await fetchBits(32, 0); // slot 0
    const B = await fetchBits(34, 1); // slots [4, 8); slot 1 and slots [2, 4) are left free
    assert.deepEqual([A.l, B.l], [at(0), at(4)]);
    await submitFor(m, A.unit_id, resultFor(A), 0);
    await submitFor(m, B.unit_id, resultFor(B), 1);
    assert.equal(await frontierOf(id), at(1), "the free slot 1 holds the frontier back");
    const C = await fetchBits(34, 2);
    assert.deepEqual([C.l, C.k], [at(1), 32], "a larger ask gets the block at the frontier");
    assert.match(C.note, /a free block of 2\^32 numbers near the frontier is smaller than the 2\^34 asked/);
    await submitFor(m, C.unit_id, resultFor(C), 2);
    assert.equal(await frontierOf(id), at(2));
    const D = await fetchBits(34, 3);
    assert.deepEqual([D.l, D.k], [at(2), 33]);
    await submitFor(m, D.unit_id, resultFor(D), 3);
    assert.equal(await frontierOf(id), at(8), "through B");
    const E = await fetchBits(34, 0);
    assert.deepEqual([E.l, E.k, E.note], [at(8), 34, null], "no hole left: the asked size at the top");
});

test("block sizes: an abandoned larger unit is dissolved for a smaller fetch, but not while a paused worker may come back", { skip }, async () => {
    const id = CAMPAIGNS.dissolve;
    const m = machines(3);
    const a = (await fetchFor(m, id, { bits: 34 }, 0)).body.lease;
    await releaseFor(m, a.unit_id, 0);
    const b = (await fetchFor(m, id, { bits: 32 }, 1)).body.lease;
    assert.equal(BigInt(b.l), BARINA_L0 + 4n * pow(32), "a released lease keeps its block until the lease would have ended");
    await srv.query(`UPDATE runs SET expires_at = ${Math.floor(Date.now() / 1000) - 1} WHERE unit_id = ${a.unit_id}`);
    const c = await fetchFor(m, id, { bits: 32 }, 2);
    measure("fetch (dissolves an abandoned larger unit)", c);
    assert.equal(c.body.lease.l, BARINA_L0.toString(), "its slots are free again");
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM units WHERE id = ${a.unit_id}`))[0].n, 0);
    const free = await srv.query(`SELECT pos, bits FROM free_blocks WHERE campaign_id = ${id} ORDER BY pos`);
    assert.deepEqual(free, [{ pos: 1, bits: 32 }, { pos: 2, bits: 33 }]);
    const back = await fetchFor(m, id, { bits: 34, prefer_unit: a.unit_id }, 0);
    assert.notEqual(back.body.lease.unit_id, a.unit_id, "the dissolved unit is gone");
    await releaseFor(m, back.body.lease.unit_id, 0);
});

test("campaigns whose l0 is not a multiple of 2^max_bits hand out no work", { skip }, async () => {
    const r = await fetchFor(machines(), CAMPAIGNS.badL0, { bits: 32 });
    assert.equal(r.status, 500);
    assert.match(r.body.error, /not a positive multiple of 2\^34/);
});
