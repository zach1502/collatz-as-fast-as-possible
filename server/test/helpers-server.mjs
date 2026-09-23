// Setup for test/server*.test.mjs. Importing this module registers before and
// after hooks in the importing test file. They run the work-unit server under
// `wrangler dev --local --env test` with a WORKER_TOKEN and a throwaway D1
// database and print the D1 rows per request. Each test uses its own
// campaign, machines and client IP (X-Test-Client-IP, honoured only with
// TEST_MODE=1), so the tests do not share state or rate-limit budgets.

import os from "node:os";
import path from "node:path";
import { after, before } from "node:test";

import { BARINA_L0, campaignSql } from "../scripts/campaign.mjs";
import { haveWrangler, moveToBlock, newHost, parseExact, readFixture, redigest, startServer, stringifyExact } from "./helpers.mjs";
import { leaseObject, leaseText, noteLease } from "./helpers-lease.mjs";

export const FIX0 = readFixture("block-k32-0.json"); // unit 0 of a K=32 campaign at 2075*2^60
export const FIX1 = readFixture("block-k32-1.json"); // block holding 274133054632352106267: wide max, 3 records
export const COMMIT = parseExact(FIX0).commit;
export const TOKEN = "server-test-token-0123456789abcdef0123456789";
const K = 32;
export const W = 1n << 32n;
export const H0 = BARINA_L0 / W;

// One block size per campaign; test/campaign.test.mjs covers block sizes and
// test/campaign-rechecks.test.mjs re-checks.
const campaign = (id, name, extra = {}) =>
    campaignSql({ id, name, minBits: K, maxBits: K, A: 10, B: 24, commits: [COMMIT], assumptionSource: "none", leaseSeconds: 3600, ...extra });
export const CAMPAIGNS = {
    basic: 1,
    expiry: 2,
    gaps: 3,
    cap: 4,
    race: 5,
    records: 6,
    window: 7,
    validate: 8,
    scan: 9,
    oldSchema: 10,
    badL0: 11,
    stored: 12,
    bigResult: 13,
};
export const RL_SECRET = "test-only-rate-limit-key-0123456789"; // wrangler.example.jsonc env.test
export const SHORT_ADMIN_SECRET = "short-admin-secret-0123456789"; // 29 characters
const SETUP = [
    campaign(1, "basic"),
    campaign(2, "expiry", { leaseSeconds: 3 }),
    campaign(3, "gaps"),
    campaign(4, "cap"),
    campaign(5, "race"),
    campaign(6, "records", {
        L0: (63826575556n * W).toString(),
        recordThreshold: "79228162514264337593543950336",
    }),
    campaign(7, "window", { maxOpen: 1 }),
    campaign(8, "validate"),
    campaign(9, "scan"),
    // A campaign of an older digest schema, which is edited in by SQL.
    campaign(10, "old schema") + "UPDATE campaigns SET digest_schema = 'collatz-result-digest/1' WHERE id = 10;\n",
    // campaignSql refuses an l0 that is not a multiple of 2^max_bits, so the l0 is edited into its SQL.
    campaign(11, "bad l0").replace(`'${BARINA_L0}'`, `'${BARINA_L0 + 1n}'`),
    campaign(12, "stored"),
    // A low record threshold, so that a result can list many records.
    campaign(13, "big result", { recordThreshold: "1" }),
].join("");

export let srv;
export const cost = [];
export const skip = !haveWrangler() && "wrangler is not installed (run npm install in server/)";
/** The local D1 state of this file's server (wrangler --persist-to), for scripts run against it. */
export const STATE_DIR = path.join(os.tmpdir(), `collatz-server-test-${process.pid}`);

before(async () => {
    if (skip) {
        return;
    }
    srv = await startServer({
        stateDir: STATE_DIR,
        setupSql: SETUP,
        // Under the 32 characters the admin API needs (test "admin API off").
        vars: { WORKER_TOKEN: TOKEN, ADMIN_SECRET: SHORT_ADMIN_SECRET },
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

export const machines = (hosts = 1) => ({ hosts: Array.from({ length: hosts }, newHost) });

export const fetchFor = async (api, m, campaignId, extra = {}, hostIdx = 0) =>
    noteLease(m.hosts[hostIdx], await api.post("/api/fetch", { token: TOKEN, host: m.hosts[hostIdx], commit: COMMIT, campaign: campaignId, ...extra }));

export const submitText = (m, unitId, docText, hostIdx = 0) =>
    `{"token":${JSON.stringify(TOKEN)},"host":"${m.hosts[hostIdx]}","unit_id":${unitId}${leaseText(m.hosts[hostIdx], unitId)},"result_json":${docText}}`;

export const submitFor = (api, m, unitId, docText, hostIdx = 0, headers = {}) =>
    api.post("/api/submit", submitText(m, unitId, docText, hostIdx), headers);

export const releaseFor = (api, m, unitId, hostIdx = 0) =>
    api.post("/api/release", { token: TOKEN, host: m.hosts[hostIdx], unit_id: unitId, ...leaseObject(m.hosts[hostIdx], unitId) });

/** Fixture 0 moved to block H0 + idx of 2^32 numbers, edited, with a matching digest. */
export function resultFor(idx, edit = () => {}) {
    const d = moveToBlock(parseExact(FIX0), H0 + BigInt(idx), K);
    edit(d);
    return stringifyExact(redigest(d));
}

export const unitOf = async (unitId) => (await srv.query(`SELECT * FROM units WHERE id = ${unitId}`))[0];
export const statsOf = async (api, id) => (await api.get(`/api/stats?campaign=${id}`)).body;
/** The block offset of a lease from the campaign start, counted in blocks of 2^32 instead of numbers. */
export const off = (lease) => Number(BigInt(lease.h) - H0);
export const at = (i) => (BARINA_L0 + BigInt(i) * W).toString();
