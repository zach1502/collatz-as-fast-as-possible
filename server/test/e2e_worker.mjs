// CTest test_integration_worker_e2e (tests/register_integration_cpu_b.cmake).
// Runs real `collatz worker` processes against the server under
// `wrangler dev --local` on a campaign at Barina's frontier 2075*2^60 (blocks
// of 2^32 to 2^34 numbers, CPU): two of the owner's machines, with the worker
// token from a file and from COLLATZ_WORKER_TOKEN. It checks that the token
// is in no file of the workers, that the second machine's audit of its unit
// reaches /api/audit, and the outcome through /api/stats.
//
//   node server/test/e2e_worker.mjs --collatz PATH --work DIR
//
// Exit 0 = pass, 1 = fail, 77 = skipped (wrangler not installed in server/).

import { execFile } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

import { BARINA_L0, campaignSql } from "../scripts/campaign.mjs";
import { client, haveWrangler, startServer } from "./helpers.mjs";

const args = process.argv.slice(2);
const opt = (name) => {
    const i = args.indexOf(name);
    return i >= 0 ? args[i + 1] : undefined;
};
const COLLATZ = opt("--collatz");
const WORK = opt("--work");
const W = 1n << 32n;

let failures = 0;
const check = (ok, what) => {
    console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
    if (!ok) {
        failures++;
    }
};

function run(file, argv, env = {}) {
    return new Promise((resolve) => {
        execFile(file, argv, { env: { ...process.env, ...env }, timeout: 600000, maxBuffer: 16 << 20 }, (err, stdout, stderr) => {
            resolve({ code: err ? (err.code ?? 1) : 0, stdout, stderr });
        });
    });
}

/** The worker's one-line reports, one per unit: only the lines that start with "unit N ". */
const lines = (out) => out.split("\n").filter((l) => /^unit \d+ /.test(l));

/** Every file under `dir` whose bytes contain `needle`. */
function filesContaining(dir, needle) {
    const hits = [];
    const walk = (d) => {
        for (const e of fs.readdirSync(d, { withFileTypes: true })) {
            const f = path.join(d, e.name);
            if (e.isDirectory()) {
                walk(f);
            } else if (e.isFile() && fs.readFileSync(f).includes(needle)) {
                hits.push(f);
            }
        }
    };
    if (fs.existsSync(dir)) {
        walk(dir);
    }
    return hits;
}

/** The workers' token checks, two machines and the --status summary; the checks are counted in `failures`. */
async function machines(srv, token) {
    const api = client(srv.url, "10.202.0.1");
    const curl = "curl";
    const cfgPath = (name) => path.join(WORK, name, "worker.json");
    // COLLATZ_WORKER_TOKEN is cleared unless a call sets it, so a value in the
    // test's own environment cannot leak in.
    const worker = (name, extra = [], env = {}) =>
        run(COLLATZ, ["worker", "--server", srv.url, "--config", cfgPath(name), "--threads", "2", "--curl", curl, ...extra], {
            COLLATZ_WORKER_BACKOFF_MS: "200",
            COLLATZ_WORKER_TOKEN: "",
            ...env,
        });
    const tokenFile = path.join(WORK, "collatz-token.txt");
    fs.writeFileSync(tokenFile, `${token}\r\n`);
    const wrongFile = path.join(WORK, "wrong-token.txt");
    fs.writeFileSync(wrongFile, `${"0".repeat(48)}\n`);
    const shortFile = path.join(WORK, "short-token.txt");
    fs.writeFileSync(shortFile, "too-short\n");

    const none = await worker("m1", ["--once"]);
    check(none.code === 2 && none.stderr.includes("--token-file"), `no token: refused\n${none.stdout}${none.stderr}`);
    const argv = await worker("m1", ["--once", "--token", token]);
    check(argv.code === 2 && argv.stderr.includes("--token-file") && !argv.stderr.includes(token), `--token on the command line is refused without echoing it\n${argv.stderr}`);
    const wrong = await worker("m1", ["--once", "--token-file", wrongFile]);
    check(wrong.code === 2 && wrong.stderr.includes("worker token"), `a wrong token is refused\n${wrong.stdout}${wrong.stderr}`);
    const short = await worker("m1", ["--once", "--token-file", shortFile]);
    check(short.code === 2 && short.stderr.includes("does not hold a worker token") && !short.stderr.includes("too-short"), `a short token file is refused\n${short.stderr}`);

    // Machine 1: the token from a file (CRLF line end).
    const m1 = await worker("m1", ["--max-units", "2", "--block-bits", "33", "--token-file", tokenFile]);
    const l1 = lines(m1.stdout);
    check(
        m1.code === 0 && l1.length === 2 && l1.every((l) => l.includes("(2^33 numbers") && l.endsWith(": accepted")),
        `machine 1: two 2^33 blocks, each accepted on its result\n${m1.stdout}${m1.stderr}`,
    );
    // Machine 2: the token from the environment, and --audit-rate 1, so its
    // unit is audited before --once exits.
    const m2 = await worker("m2", ["--once", "--block-bits", "32", "--audit-rate", "1"], { COLLATZ_WORKER_TOKEN: token });
    check(m2.code === 0 && lines(m2.stdout)[0]?.endsWith(": accepted"), `machine 2: a 2^32 block from COLLATZ_WORKER_TOKEN\n${m2.stdout}${m2.stderr}`);
    check(/^audit of unit \d+ \(16384 residues, seed [0-9a-f]{16}\.\.\.\): 0 failures, /m.test(m2.stdout), `machine 2 audits its unit\n${m2.stdout}${m2.stderr}`);
    const m2log = fs.readFileSync(path.join(WORK, "m2", "worker.log"), "utf8");
    check(/the audit of unit \d+ was reported to the server/.test(m2log), `the audit reached /api/audit\n${m2log}`);
    const stored = await srv.query("SELECT r.digest = a.seed AS same, a.user, a.samples, a.failures FROM audits a JOIN runs r ON r.unit_id = a.unit_id AND r.host = a.host");
    check(
        stored.length === 1 && stored[0].same === 1 && stored[0].user === "owner" && stored[0].samples === 16384 && stored[0].failures === 0,
        `the server keeps the audit of the run's result (${JSON.stringify(stored)})`,
    );
    check(!fs.existsSync(path.join(WORK, "m2", "audits")) || fs.readdirSync(path.join(WORK, "m2", "audits")).length === 0, "a clean audit leaves no files");

    const s = (await api.get("/api/stats?campaign=1")).body;
    check(
        s.owner_name === "E2E Owner" && s.totals.blocks === 3 && s.totals.numbers_verified === (5n * W).toString(),
        `stats: 3 blocks, 2^34 + 2^32 numbers (${JSON.stringify(s.totals)})`,
    );
    check(s.frontier.value === (BARINA_L0 + 5n * W).toString(), `frontier = 2075*2^60 + 5*2^32 (got ${s.frontier.value})`);
    const hosts = s.hosts.map((h) => h.host).sort();
    const ids = [JSON.parse(fs.readFileSync(cfgPath("m1"), "utf8")).host_id, JSON.parse(fs.readFileSync(cfgPath("m2"), "utf8")).host_id].sort();
    check(hosts.join(",") === ids.join(","), `stats list both machines by host id (${hosts.join(",")})`);
    check(
        s.blocks.length === 3 && s.blocks.every((b) => b.seconds > 0 && b.numbers_per_second > 0),
        `every block reports its time and speed (${JSON.stringify(s.blocks.map((b) => [b.k, b.seconds]))})`,
    );
    check(s.speed.last_10_min.blocks === 3, "the last 10 minutes hold the three blocks");
    check(JSON.stringify(s.audits) === JSON.stringify({ passed: 1, failed: 0 }), `the page counts the audit (${JSON.stringify(s.audits)})`);

    // The token is in no file of the workers.
    const leaked = [...filesContaining(path.join(WORK, "m1"), token), ...filesContaining(path.join(WORK, "m2"), token)];
    check(leaked.length === 0, `the token is in no file of the workers (${leaked.join(", ")})`);
    const cfg = fs.readFileSync(cfgPath("m1"), "utf8");
    check(!cfg.includes("token") && !cfg.includes(token), "the config file stores no token");

    // --status prints the campaign's summary from /api/stats.
    const st = await run(COLLATZ, ["worker", "--status", "--server", srv.url, "--config", path.join(WORK, "status.json")]);
    check(
        st.code === 0 &&
            st.stdout.includes("campaign e2e-owner (id 1)") &&
            st.stdout.includes(`frontier: ${BARINA_L0 + 5n * W} (2^`) &&
            st.stdout.includes(`numbers checked: ${5n * W} `) &&
            st.stdout.includes("speed (24 h): ") &&
            st.stdout.includes("machines: 2"),
        `--status reads /api/stats\n${st.stdout}${st.stderr}`,
    );
}

if (!COLLATZ || !WORK) {
    console.error("usage: node e2e_worker.mjs --collatz PATH --work DIR");
    process.exit(2);
}
if (!haveWrangler()) {
    console.log("skip: wrangler is not installed (run npm install in server/)");
    process.exit(77);
}

fs.rmSync(WORK, { recursive: true, force: true });
fs.mkdirSync(WORK, { recursive: true });

// The commit the binary reports is the one the campaign must accept.
const probe = path.join(WORK, "probe.json");
const p = await run(COLLATZ, ["verify", "--N", "20", "--A", "0", "--json", probe]);
if (p.code !== 0) {
    console.error(`collatz verify failed:\n${p.stderr}`);
    process.exit(1);
}
const commit = JSON.parse(fs.readFileSync(probe, "utf8")).commit;
console.log(`binary commit ${commit}`);

const token = crypto.randomBytes(24).toString("hex");
const srv = await startServer({
    stateDir: path.join(WORK, "server"),
    setupSql: campaignSql({ id: 1, name: "e2e-owner", minBits: 32, maxBits: 34, A: 10, B: 24, commits: [commit], leaseSeconds: 3600 }),
    vars: { WORKER_TOKEN: token, OWNER_NAME: "E2E Owner" },
});
try {
    await machines(srv, token);
} finally {
    await srv.stop();
}
if (failures > 0) {
    console.error(`${failures} check(s) failed; server log: ${srv.logPath}`);
    process.exit(1);
}
console.log("all worker end-to-end checks passed");
