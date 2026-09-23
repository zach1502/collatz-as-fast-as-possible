// Shared by the campaign tests (test/campaign.test.mjs and
// test/campaign-*.test.mjs). Each file runs the Worker under
// `wrangler dev --local --env test` with a WORKER_TOKEN, a 7-day embargo (so
// that re-checks can be offered) and RECHECK_EVERY=2. Each test uses its own
// campaign, machines and client IP; `due` makes a campaign's re-checks due
// without running RECHECK_EVERY units first.

import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { after, before } from "node:test";

import { BARINA_L0, campaignSql } from "../scripts/campaign.mjs";
import { SERVER_DIR, client, haveWrangler, newHost, parseExact, readFixture, resultForLease, startServer } from "./helpers.mjs";
import { leaseObject, leaseText, noteLease } from "./helpers-lease.mjs";

export const FIX0 = readFixture("block-k32-0.json");
const COMMIT = parseExact(FIX0).commit;
export const TOKEN = "campaign-test-token-0123456789abcdef0123456789";
const ADMIN_SECRET = "test-only-admin-secret-0123456789"; // wrangler.example.jsonc env.test
export const DAY = 86400;

export const CAMPAIGNS = {
    q1: 1,
    sizes: 2,
    fallback: 3,
    gaps: 4,
    rate: 5,
    prefHost: 6,
    prefBackend: 7,
    prefSame: 8,
    conflict: 9,
    pacing: 10,
    idle: 11,
    dissolve: 12,
    badL0: 13,
    decide: 14,
    small: 15,
    embargo: 16,
    withdraw: 17,
    requeue: 18,
    leaseRequeue: 19,
    leaseRecheck: 20,
    leaseTaken: 21,
    leaseDiffer: 22,
    leaseNone: 23,
    verifierRange: 24,
    hole: 25,
};
/**
 * The start of a campaign of 2^60 blocks at A = 10 whose first block is the
 * last one verify takes: the next ends past (H+1)*2^K*3^M < 2^(125+M), M = 50.
 */
export const RANGE_L0 = ((((1n << 175n) - 1n) / 3n ** 50n >> 60n) - 1n) << 60n;
const campaign = (id, name, o) =>
    campaignSql({ id, name, A: 10, B: 24, commits: [COMMIT], assumptionSource: "none", leaseSeconds: 3600, ...o });
const SETUP = [
    campaign(CAMPAIGNS.q1, "q1", { minBits: 32, maxBits: 34 }),
    campaign(CAMPAIGNS.sizes, "sizes", { minBits: 46, maxBits: 50 }),
    campaign(CAMPAIGNS.fallback, "fallback", { minBits: 46, maxBits: 50 }),
    campaign(CAMPAIGNS.gaps, "gaps", { minBits: 32, maxBits: 34 }),
    campaign(CAMPAIGNS.rate, "rate", { minBits: 32, maxBits: 32, recheckRate: 0 }),
    campaign(CAMPAIGNS.prefHost, "pref host", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.prefBackend, "pref backend", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.prefSame, "pref same", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.conflict, "conflict", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.pacing, "pacing", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.idle, "idle", { minBits: 32, maxBits: 32, recheckRate: 1, end: (BARINA_L0 + (1n << 32n)).toString() }),
    campaign(CAMPAIGNS.dissolve, "dissolve", { minBits: 32, maxBits: 34 }),
    // campaignSql refuses an l0 that is not a multiple of 2^max_bits, so the l0 is edited into its SQL.
    campaign(CAMPAIGNS.badL0, "bad l0", { minBits: 32, maxBits: 34 }).replace(`'${BARINA_L0}'`, `'${BARINA_L0 + (1n << 33n)}'`),
    campaign(CAMPAIGNS.decide, "decide", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.small, "small units", { minBits: 32, maxBits: 42 }),
    campaign(CAMPAIGNS.embargo, "embargo", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.withdraw, "withdraw", { minBits: 32, maxBits: 33 }),
    campaign(CAMPAIGNS.requeue, "requeue", { minBits: 32, maxBits: 33, recheckRate: 1 }),
    campaign(CAMPAIGNS.leaseRequeue, "lease requeue", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.leaseRecheck, "lease recheck", { minBits: 32, maxBits: 32, recheckRate: 1 }),
    campaign(CAMPAIGNS.leaseTaken, "lease taken", { minBits: 32, maxBits: 32 }),
    campaign(CAMPAIGNS.leaseDiffer, "lease differ", { minBits: 32, maxBits: 32 }),
    campaign(CAMPAIGNS.leaseNone, "lease none", { minBits: 32, maxBits: 32 }),
    campaign(CAMPAIGNS.hole, "hole", { minBits: 32, maxBits: 36 }),
    campaign(CAMPAIGNS.verifierRange, "verifier range", { minBits: 60, maxBits: 60, L0: RANGE_L0.toString() }),
].join("");

export let srv;
const cost = [];
export const skip = !haveWrangler() && "wrangler is not installed (run npm install in server/)";

/**
 * Registers the hooks of a campaign test file: before its tests the Worker
 * starts with every campaign of SETUP; after them it stops, and the D1 rows
 * of each measured request are printed.
 */
export function serveCampaigns() {
    before(async () => {
        if (skip) {
            return;
        }
        srv = await startServer({
            stateDir: path.join(os.tmpdir(), `collatz-server-campaign-test-${process.pid}`),
            setupSql: SETUP,
            vars: { WORKER_TOKEN: TOKEN, EMBARGO_SECONDS: "604800", RECHECK_EVERY: "2" },
        });
    });

    after(async () => {
        await srv?.stop();
        if (cost.length > 0) {
            console.log("D1 rows per request, block sizes and re-checks (read / written):");
            for (const c of cost) {
                console.log(`  ${c.what.padEnd(40)} ${String(c.read).padStart(4)} / ${c.written}`);
            }
        }
    });
}

export const measure = (what, r) => cost.push({ what, read: r.rowsRead, written: r.rowsWritten });
let ipSeq = 0;
export const newApi = () => client(srv.url, `10.4.${Math.floor(++ipSeq / 250)}.${(ipSeq % 250) + 1}`);

export const machines = (hosts = 1) => ({ api: newApi(), hosts: Array.from({ length: hosts }, newHost) });

export const fetchFor = async (m, campaignId, extra = {}, hostIdx = 0) =>
    noteLease(m.hosts[hostIdx], await m.api.post("/api/fetch", { token: TOKEN, host: m.hosts[hostIdx], commit: COMMIT, campaign: campaignId, ...extra }));

/**
 * A submit body as text, so that the result keeps its exact u64 counters: on
 * host `hostIdx`'s own lease of the unit, or on lease `leaseId` when given.
 */
export const submitBody = (m, unitId, docText, hostIdx = 0, leaseId = undefined) =>
    `{"token":${JSON.stringify(TOKEN)},"host":"${m.hosts[hostIdx]}","unit_id":${unitId}` +
    `${leaseId === undefined ? leaseText(m.hosts[hostIdx], unitId) : `,"lease_id":${leaseId}`},"result_json":${docText}}`;

export const submitFor = (m, unitId, docText, hostIdx = 0, headers = {}) => m.api.post("/api/submit", submitBody(m, unitId, docText, hostIdx), headers);

export const releaseFor = (m, unitId, hostIdx = 0) =>
    m.api.post("/api/release", { token: TOKEN, host: m.hosts[hostIdx], unit_id: unitId, ...leaseObject(m.hosts[hostIdx], unitId) });

export const auditFor = (m, unitId, seed, o = {}, hostIdx = 0) =>
    m.api.post("/api/audit", { token: TOKEN, host: m.hosts[hostIdx], unit_id: unitId, seed, samples: 1000, failures: 0, seconds: 1, ...o });

export const resultFor = (lease, edit) => resultForLease(FIX0, lease, edit);
export const wrongFor = (lease) => resultFor(lease, (d) => (d.result.blocks_total = Number(d.result.blocks_total) + 1));
export const onGpu = (lease) => resultFor(lease, (d) => (d.params.gpu = true));
export const digestOf = (lease) => parseExact(resultFor(lease)).result.result_digest;

/** Fetches a lease, submits the right result of its block, and returns both answers. */
export async function runOne(m, campaignId, extra = {}, hostIdx = 0, doc = resultFor, headers = {}) {
    const f = await fetchFor(m, campaignId, extra, hostIdx);
    assert.equal(f.status, 200, f.text);
    assert.notEqual(f.body.lease, null, f.text);
    const s = await submitFor(m, f.body.lease.unit_id, doc(f.body.lease), hostIdx, headers);
    assert.equal(s.status, 200, s.text);
    return { lease: f.body.lease, submit: s.body, fetchRes: f, submitRes: s };
}

export const unitRow = async (unitId) => (await srv.query(`SELECT * FROM units WHERE id = ${unitId}`))[0];
export const campaignRow = async (id) => (await srv.query(`SELECT * FROM campaigns WHERE id = ${id}`))[0];
export const statsOf = async (id) => (await newApi().get(`/api/stats?campaign=${id}`)).body;
export const frontierOf = async (id) => (await statsOf(id)).frontier.value;
export const span = (l) => [BigInt(l.l), BigInt(l.end)];
export const pow = (k) => 1n << BigInt(k);
/** Sets the campaign's since_recheck to 99, so its waiting re-checks are due (as after RECHECK_EVERY regular results). */
export const due = (id) =>
    srv.query(
        `INSERT INTO user_stats (campaign_id, user, since_recheck, last_at) VALUES (${id}, 'owner', 99, 0)
         ON CONFLICT (campaign_id, user) DO UPDATE SET since_recheck = 99`,
    );
export const admin = (method, p, body) =>
    method === "GET"
        ? newApi().get(p, { authorization: `Bearer ${ADMIN_SECRET}` })
        : newApi().post(p, body, { authorization: `Bearer ${ADMIN_SECRET}` });
export const nowS = () => Math.floor(Date.now() / 1000);
/** scripts/admin.mjs against the test server; resolves to {code, stdout, stderr}. */
export const adminScript = (args) =>
    new Promise((resolve) => {
        execFile(
            process.execPath,
            [path.join(SERVER_DIR, "scripts", "admin.mjs"), "--server", srv.url, ...args],
            { env: { ...process.env, ADMIN_SECRET } },
            (err, stdout, stderr) => resolve({ code: err ? err.code : 0, stdout, stderr }),
        );
    });

/** Makes host `hostIdx` of `m` active in the campaign as a fetch would, but without a lease. */
export const activate = (id, m, hostIdx = 0) =>
    srv.query(
        `INSERT INTO hosts (campaign_id, user, host, backend, recheck_cpu, bits, last_seen)
         VALUES (${id}, 'owner', '${m.hosts[hostIdx]}', 'cpu', 0, 64, ${nowS()})
         ON CONFLICT (campaign_id, user, host) DO UPDATE SET last_seen = excluded.last_seen`,
    );
export const ledgerOf = async (id) => (await newApi().get("/api/ledger?limit=1000")).body.filter((e) => e.campaign === id);
