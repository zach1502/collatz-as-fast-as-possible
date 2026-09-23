// Shared by the tests of the public ledger (src/ledger.ts,
// schema/0004_ledger.sql) and its check (scripts/verify-ledger.mjs):
// test/ledger.test.mjs and test/ledger-*.test.mjs. A test file runs the
// Worker under `wrangler dev --local --env test` with a
// WORKER_TOKEN, the 7-day embargo and RECHECK_EVERY=0 (as in
// test/owner.test.mjs). Every hash is checked against the canonical line
// rebuilt here; each test looks at the entries of its own campaigns, and the
// tests of one server run one after another.

import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { after, before } from "node:test";

import { campaignSql } from "../scripts/campaign.mjs";
import {
    SERVER_DIR,
    client,
    haveWrangler,
    parseExact,
    readFixture,
    resultForLease,
    sha256Hex,
    startServer,
} from "./helpers.mjs";
import { leaseText, noteLease } from "./helpers-lease.mjs";

export const FIX0 = readFixture("block-k32-0.json");
export const COMMIT = parseExact(FIX0).commit;
export const SCHEMA = "collatz-result-digest/2";
const TOKEN = "ledger-test-token-0123456789abcdef0123456789";
const ADMIN_SECRET = "test-only-admin-secret-0123456789";
export const ADMIN = { authorization: `Bearer ${ADMIN_SECRET}` };
export const GENESIS = "0".repeat(64);
/** The fields of a collatz-ledger/1 line after the format; /2 adds who and votes before prev. */
const V1_FIELDS = ["seq", "campaign", "at", "kind", "start", "bits", "end", "result", "peak", "peak_n", "seconds", "machine", "backend", "version", "schema", "prev"];
const V2_FIELDS = [...V1_FIELDS.slice(0, -1), "who", "votes", "prev"];
/** The fields of a published entry, in order. */
export const COLUMNS = ["format", ...V2_FIELDS, "hash"];
export const W = 1n << 32n;

export const OWNER = { accept: 1, chain: 2, recheck: 3, conflict: 4, v1: 5 };
const base = { minBits: 32, maxBits: 32, A: 10, B: 24, commits: [COMMIT], assumptionSource: "none", leaseSeconds: 3600 };
const OWNER_SETUP = [
    campaignSql({ ...base, id: OWNER.accept, name: "accept", minBits: 32, maxBits: 34 }),
    campaignSql({ ...base, id: OWNER.chain, name: "chain" }),
    campaignSql({ ...base, id: OWNER.recheck, name: "recheck", recheckRate: 1 }),
    campaignSql({ ...base, id: OWNER.conflict, name: "conflict", recheckRate: 1 }),
    campaignSql({ ...base, id: OWNER.v1, name: "v1" }),
].join("");

export let owner;
export const skip = !haveWrangler() && "wrangler is not installed (run npm install in server/)";
const cost = [];
export const measure = (what, r) => cost.push({ what, read: r.rowsRead, written: r.rowsWritten });

/**
 * Registers the hooks of a ledger test file: the Worker starts before the
 * file's tests and stops after them, and the D1 rows of each measured
 * request are printed.
 */
export function serveLedgerOwner() {
    before(async () => {
        if (skip) {
            return;
        }
        owner = await startServer({
            stateDir: path.join(os.tmpdir(), `collatz-server-ledger-owner-test-${process.pid}`),
            setupSql: OWNER_SETUP,
            // The admin secret as `wrangler secret put` stores a file's line: with its newline.
            vars: { WORKER_TOKEN: TOKEN, OWNER_NAME: "Test Owner", EMBARGO_SECONDS: "604800", RECHECK_EVERY: "0", ADMIN_SECRET: `${ADMIN_SECRET}\n` },
        });
    });

    after(async () => {
        await owner?.stop();
        if (cost.length > 0) {
            console.log("D1 rows per request, ledger (read / written):");
            for (const c of cost) {
                console.log(`  ${c.what.padEnd(52)} ${String(c.read).padStart(4)} / ${c.written}`);
            }
        }
    });
}

// Every client gets its own address, so the per-address rate limit never trips.
let ipSeq = 0;
export const newApi = (srv) => {
    ipSeq++;
    return client(srv.url, `10.9.${Math.floor(ipSeq / 250)}.${(ipSeq % 250) + 1}`);
};
export const nowS = () => Math.floor(Date.now() / 1000);

/** The canonical line of an entry, rebuilt from its fields in the order its format lists them, so its hash can be checked. */
export const lineOf = (e) => [e.format, ...(e.format === "collatz-ledger/1" ? V1_FIELDS : V2_FIELDS).map((k) => e[k])].join("|");

/** The head and every entry of the ledger of `srv`, read page by page. */
export async function readLedger(srv) {
    const api = newApi(srv);
    const head = (await api.get("/api/ledger/head")).body;
    const entries = [];
    while (entries.length < head.seq) {
        const r = await api.get(`/api/ledger?from=${entries.length + 1}&limit=1000`);
        assert.equal(r.status, 200, r.text);
        assert.ok(r.body.length > 0, "a page below the head has entries");
        entries.push(...r.body);
    }
    return { head, entries };
}

/** Checks that seq counts from 1, each prev is the hash before it, each hash is its line's and the head is the last entry. */
export function assertChain({ head, entries }) {
    let prev = GENESIS;
    for (const [i, e] of entries.entries()) {
        assert.deepEqual(Object.keys(e), COLUMNS, `entry ${i + 1}`);
        assert.equal(e.seq, i + 1, "no gaps");
        assert.equal(e.prev, prev, `entry ${e.seq} links to the one before it`);
        assert.equal(e.hash, sha256Hex(lineOf(e)), `entry ${e.seq} is hashed over its line`);
        prev = e.hash;
    }
    const last = entries[entries.length - 1];
    assert.deepEqual(head, { seq: entries.length, hash: prev, at: last === undefined ? null : last.at });
}

/** The whole chain of `srv`, checked, and the entries of campaign `id`. */
export async function entriesOf(srv, id) {
    const ledger = await readLedger(srv);
    assertChain(ledger);
    return ledger.entries.filter((e) => e.campaign === id);
}

export const resultFields = (e) => ({ result: e.result, peak: e.peak, peak_n: e.peak_n, seconds: e.seconds, machine: e.machine, backend: e.backend, version: e.version });

/** The document text with seconds_total written as `raw`; the digest does not bind it, so it stays valid. */
export const withSeconds = (text, raw) => text.replace(/("seconds_total":\s*)[0-9.]+/, `$1${raw}`);
/** An edit that puts blocks_total one too high; resultFor redigests it, so the digest matches the wrong result. */
export const wrong = (d) => (d.result.blocks_total = Number(String(d.result.blocks_total).replaceAll("@", "")) + 1);

export const fetchAs = async (api, host, campaignId, extra = {}) =>
    noteLease(host, await api.post("/api/fetch", { token: TOKEN, host, commit: COMMIT, campaign: campaignId, ...extra }));
export const submitAs = (api, host, unitId, docText, headers = {}) =>
    api.post("/api/submit", `{"token":${JSON.stringify(TOKEN)},"host":"${host}","unit_id":${unitId}${leaseText(host, unitId)},"result_json":${docText}}`, headers);

export const resultFor = (lease, edit) => resultForLease(FIX0, lease, edit);

/** Fetches a lease on `host` and submits its result; returns the lease and the submitted document. */
export async function runOne(api, host, campaignId, { extra = {}, edit = () => {}, seconds } = {}) {
    const f = await fetchAs(api, host, campaignId, extra);
    assert.equal(f.status, 200, f.text);
    assert.notEqual(f.body.lease, null, f.text);
    const text = seconds === undefined ? resultFor(f.body.lease, edit) : withSeconds(resultFor(f.body.lease, edit), seconds);
    const s = await submitAs(api, host, f.body.lease.unit_id, text);
    assert.equal(s.status, 200, s.text);
    assert.equal(s.body.state, "accepted");
    return { lease: f.body.lease, doc: parseExact(text) };
}

export const frontierOf = async (id) => (await newApi(owner).get(`/api/stats?campaign=${id}`)).body.frontier.value;

/** Runs scripts/`name` with `args`; resolves to {code, stdout, stderr} and never rejects, so a test can check a failed run. */
export const runScript = (name, args) =>
    new Promise((resolve) => {
        execFile(process.execPath, [path.join(SERVER_DIR, "scripts", name), ...args], { cwd: SERVER_DIR, maxBuffer: 64 << 20 }, (err, stdout, stderr) =>
            resolve({ code: err === null ? 0 : err.code, stdout, stderr }),
        );
    });
export const isoOf = (at) => new Date(at * 1000).toISOString().replace(/\.000Z$/, "Z");
