// Shared by test/owner.test.mjs and test/owner-*.test.mjs. Each file runs the
// Worker under `wrangler dev --local --env test` with a WORKER_TOKEN,
// OWNER_NAME, the 7-day embargo (so that re-checks can be offered) and
// RECHECK_EVERY=0. Each test uses its own campaign and client IP, and reads
// the units' state from the database.

import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { after, before } from "node:test";

import { RECORD97_PEAK_T, campaignSql } from "../scripts/campaign.mjs";
import { client, haveWrangler, parseExact, readFixture, resultForLease, startServer } from "./helpers.mjs";
import { leaseText, noteLease } from "./helpers-lease.mjs";

const FIX0 = readFixture("block-k32-0.json");
export const COMMIT = parseExact(FIX0).commit;
export const TOKEN = "owner-test-token-0123456789abcdef0123456789";
export const OWNER_NAME = "Test Owner";
export const ADMIN_SECRET = "test-only-admin-secret-0123456789"; // wrangler.example.jsonc env.test
export const pow = (k) => 1n << BigInt(k);

export const CAMPAIGNS = {
    auth: 1,
    accept: 2,
    q2: 3,
    stats: 4,
    math: 5,
    recheck: 6,
    cache: 7,
    hosts: 8,
    page: 9,
    paths: 10,
    pathsMax: 11,
    pathsBack: 12,
    pathsOff: 13,
    pathsHigh: 14,
    audits: 15,
    buckets: 16,
};
/** Path record #97, the last one below 2^71: its T-map peak is the default record threshold. */
export const T97 = BigInt(RECORD97_PEAK_T);
export const N97 = "2358909599867980429759";
/** #97's peak as Barina's path-records list gives it, in the 3n+1 map. */
export const PEAK97 = "988229361387264324637139131613488937796184";
/** Below #97's T-map peak, so that a unit also lists walks that do not beat #97. */
export const LOW_THRESHOLD = (T97 - 10n ** 20n).toString();
const campaign = (id, name, o) =>
    campaignSql({ id, name, A: 10, B: 24, commits: [COMMIT], assumptionSource: "none", leaseSeconds: 3600, ...o });
const SETUP = [
    campaign(CAMPAIGNS.auth, "auth", { minBits: 32, maxBits: 34 }),
    campaign(CAMPAIGNS.accept, "accept", { minBits: 32, maxBits: 34 }),
    // campaign.mjs writes quorum 1 only, so the quorum is edited in by SQL.
    campaign(CAMPAIGNS.q2, "q2", { minBits: 32, maxBits: 32 }) + `UPDATE campaigns SET quorum = 2 WHERE id = ${CAMPAIGNS.q2};\n`,
    campaign(CAMPAIGNS.stats, "stats", { minBits: 32, maxBits: 34 }),
    campaign(CAMPAIGNS.math, "math", { minBits: 32, maxBits: 34 }),
    campaign(CAMPAIGNS.recheck, "recheck", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.cache, "cache", { minBits: 32, maxBits: 32 }),
    campaign(CAMPAIGNS.hosts, "hosts", { minBits: 32, maxBits: 32 }),
    // The defaults of a real campaign at 2075*2^60: the assumption source that
    // cites Barina, the record threshold, the kernel, the digest schema.
    campaign(CAMPAIGNS.page, "page", { minBits: 32, maxBits: 32, assumptionSource: undefined }),
    campaign(CAMPAIGNS.paths, "paths", { minBits: 32, maxBits: 32 }),
    campaign(CAMPAIGNS.pathsMax, "paths max", { minBits: 32, maxBits: 32, recordThreshold: LOW_THRESHOLD }),
    campaign(CAMPAIGNS.pathsBack, "paths back", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.pathsOff, "paths off", { minBits: 32, maxBits: 32, recordThreshold: "none" }),
    campaign(CAMPAIGNS.pathsHigh, "paths high", { minBits: 32, maxBits: 32, recordThreshold: (T97 + 1n).toString() }),
    campaign(CAMPAIGNS.audits, "audits", { minBits: 32, maxBits: 32 }),
    campaign(CAMPAIGNS.buckets, "buckets", { minBits: 32, maxBits: 32 }),
].join("");

export let srv;
const cost = [];
export const skip = !haveWrangler() && "wrangler is not installed (run npm install in server/)";

/**
 * Registers the hooks of a test file: before its tests the Worker starts with
 * every campaign of SETUP; after them it stops, and the D1 rows of each
 * measured request are printed.
 */
export function serveOwner() {
    before(async () => {
        if (skip) {
            return;
        }
        srv = await startServer({
            stateDir: path.join(os.tmpdir(), `collatz-server-owner-test-${process.pid}`),
            setupSql: SETUP,
            vars: { WORKER_TOKEN: TOKEN, OWNER_NAME, EMBARGO_SECONDS: "604800", RECHECK_EVERY: "0" },
        });
    });

    after(async () => {
        await srv?.stop();
        if (cost.length > 0) {
            console.log("D1 rows per request (read / written):");
            for (const c of cost) {
                console.log(`  ${c.what.padEnd(36)} ${String(c.read).padStart(4)} / ${c.written}`);
            }
        }
    });
}

export const measure = (what, r) => cost.push({ what, read: r.rowsRead, written: r.rowsWritten });
let ipSeq = 0;
export const newApi = () => client(srv.url, `10.6.${Math.floor(++ipSeq / 250)}.${(ipSeq % 250) + 1}`);

export const fetchAs = async (api, host, campaignId, extra = {}, token = TOKEN) =>
    noteLease(host, await api.post("/api/fetch", { token, host, commit: COMMIT, campaign: campaignId, ...extra }));
export const submitAs = (api, host, unitId, docText, token = TOKEN, headers = {}) =>
    api.post("/api/submit", `{"token":${JSON.stringify(token)},"host":"${host}","unit_id":${unitId}${leaseText(host, unitId)},"result_json":${docText}}`, headers);

export const resultFor = (lease, edit) => resultForLease(FIX0, lease, edit);

/** Fetches a lease on `host`, submits the right result of its block (edited), and returns both answers. */
export async function runOne(api, host, campaignId, extra = {}, edit = () => {}, headers = {}) {
    const f = await fetchAs(api, host, campaignId, extra);
    assert.equal(f.status, 200, f.text);
    assert.notEqual(f.body.lease, null, f.text);
    const s = await submitAs(api, host, f.body.lease.unit_id, resultFor(f.body.lease, edit), TOKEN, headers);
    assert.equal(s.status, 200, s.text);
    return { lease: f.body.lease, submit: s.body, fetchRes: f, submitRes: s };
}

export const statsOf = async (id, headers = {}) => {
    const r = await newApi().get(`/api/stats?campaign=${id}`, headers);
    assert.equal(r.status, 200, r.text);
    return r;
};
export const nowS = () => Math.floor(Date.now() / 1000);
export const close = (a, b, what) => assert.ok(Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(b)), `${what}: ${a} vs ${b}`);
export const sum = (xs) => xs.reduce((a, b) => a + b, 0n);
/** log2 of a positive BigInt from its top 53 bits. */
export const log2Of = (x) => {
    const shift = Math.max(0, x.toString(2).length - 53);
    return shift + Math.log2(Number(x >> BigInt(shift)));
};
export const rec = (r) => [r.number, r.n, r.peak, r.known, r.found_at];
export const REC97 = [97, N97, PEAK97, true, null];
/** The path record row of a hit with T-map peak `peakT`, found at `at`; its peak is twice that because records give 3n+1-map peaks. */
export const recOf = (number, n, peakT, at) => [number, n, (2n * peakT).toString(), false, at];
export const acceptedAt = async (unitId) => (await srv.query(`SELECT accepted_at FROM units WHERE id = ${unitId}`))[0].accepted_at;
/** Edits a result to list `hits` ([n, T-map peak] by offset from the block start, ascending) with the largest as its maximum. */
export const withHits = (hits) => (d) => {
    const L = BigInt(d.block.L);
    d.result.records = hits.map(([off, peakT]) => [(L + off).toString(), peakT.toString()]);
    const [off, top] = hits.reduce((a, h) => (h[1] > a[1] ? h : a));
    d.result.max_excursion = top.toString();
    d.result.max_excursion_n = (L + off).toString();
};
