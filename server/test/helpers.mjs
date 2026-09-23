// Shared by the npm tests that run a Worker and test/e2e_worker.mjs (CTest):
// runs the Worker under `wrangler dev --local --env test` on free ports with a
// throwaway D1 database, plus an API client and helpers that rewrite result
// fixtures. Every wrangler command reads wrangler.example.jsonc, never the
// git-ignored wrangler.jsonc that holds the owner's live settings.

import { execFile, spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const SERVER_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const WRANGLER = path.join(SERVER_DIR, "node_modules", "wrangler", "bin", "wrangler.js");
const CONFIG = path.join(SERVER_DIR, "wrangler.example.jsonc");

export function haveWrangler() {
    return fs.existsSync(WRANGLER);
}

const childEnv = () => ({ ...process.env, WRANGLER_SEND_METRICS: "false", CI: "1", NO_COLOR: "1" });

function freePort() {
    return new Promise((resolve, reject) => {
        const s = net.createServer();
        s.unref();
        s.on("error", reject);
        s.listen(0, "127.0.0.1", () => {
            const { port } = s.address();
            s.close(() => resolve(port));
        });
    });
}

function wrangler(args) {
    return new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [WRANGLER, ...args],
            { cwd: SERVER_DIR, env: childEnv(), maxBuffer: 16 << 20, timeout: 300000 },
            (err, stdout, stderr) => {
                if (err) {
                    reject(new Error(`wrangler ${args.join(" ")} failed: ${err.message}\n${stdout}\n${stderr}`));
                } else {
                    resolve(stdout);
                }
            },
        );
    });
}

/**
 * Applies the migrations to a fresh local database in `stateDir`, runs
 * `setupSql` (campaigns), and starts `wrangler dev`; `vars` override the
 * `test` environment's vars (wrangler dev --var NAME:VALUE). Returns {url,
 * stop, sql, query, logPath}.
 */
export async function startServer({ stateDir, setupSql = "", log = false, vars = {} }) {
    fs.rmSync(stateDir, { recursive: true, force: true });
    fs.mkdirSync(stateDir, { recursive: true });
    const common = ["--config", CONFIG, "--local", "--env", "test", "--persist-to", stateDir];
    await wrangler(["d1", "migrations", "apply", "DB", ...common]);
    const runSql = async (text) => {
        const file = path.join(stateDir, `sql-${crypto.randomUUID()}.sql`);
        fs.writeFileSync(file, text);
        try {
            return await wrangler(["d1", "execute", "DB", ...common, "--file", file, "--yes"]);
        } finally {
            fs.rmSync(file, { force: true });
        }
    };
    if (setupSql !== "") {
        await runSql(setupSql);
    }
    // One SQL statement against the same database; resolves to its result rows.
    const query = async (text) => {
        const out = await wrangler(["d1", "execute", "DB", ...common, "--command", text, "--json", "--yes"]);
        const parsed = JSON.parse(out.slice(out.indexOf("[")));
        return parsed[0]?.results ?? [];
    };
    const port = await freePort();
    const inspector = await freePort();
    const logPath = path.join(stateDir, "wrangler-dev.log");
    const logFd = fs.openSync(logPath, "w");
    const child = spawn(
        process.execPath,
        [
            WRANGLER,
            "dev",
            ...common,
            "--ip",
            "127.0.0.1",
            "--port",
            String(port),
            "--inspector-port",
            String(inspector),
            "--show-interactive-dev-session=false",
            "--log-level",
            log ? "log" : "warn",
            ...Object.entries(vars).flatMap(([k, v]) => ["--var", `${k}:${v}`]),
        ],
        { cwd: SERVER_DIR, env: childEnv(), stdio: ["ignore", logFd, logFd] },
    );
    const url = `http://127.0.0.1:${port}`;
    let exited = null;
    child.on("exit", (code) => {
        exited = code;
    });
    const deadline = Date.now() + 180000;
    for (;;) {
        if (exited !== null) {
            throw new Error(`wrangler dev exited (${exited}):\n${fs.readFileSync(logPath, "utf8")}`);
        }
        try {
            // /api/ledger/head answers without a campaign or a token.
            const r = await fetch(`${url}/api/ledger/head`, { headers: { "x-test-client-ip": "startup" } });
            if (r.status === 200) {
                break;
            }
        } catch {
            // not listening yet
        }
        if (Date.now() > deadline) {
            child.kill("SIGKILL");
            throw new Error(`wrangler dev did not come up:\n${fs.readFileSync(logPath, "utf8")}`);
        }
        await new Promise((r) => setTimeout(r, 500));
    }
    const stop = async () => {
        if (exited !== null) {
            return;
        }
        const done = new Promise((r) => child.on("exit", r));
        child.kill("SIGTERM");
        const t = setTimeout(() => child.kill("SIGKILL"), 10000);
        await done;
        clearTimeout(t);
        fs.closeSync(logFd);
    };
    return { url, stop, sql: runSql, query, logPath };
}

/** POST/GET helper: resolves to {status, body, rowsRead, rowsWritten, text, headers}. */
export function client(url, ip = "10.0.0.1") {
    const call = async (method, p, body, headers = {}) => {
        const r = await fetch(`${url}${p}`, {
            method,
            headers: { "content-type": "application/json", "x-test-client-ip": ip, ...headers },
            body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
        });
        const text = await r.text();
        let parsed = null;
        try {
            parsed = JSON.parse(text);
        } catch {
            // non-JSON (static files)
        }
        return {
            status: r.status,
            body: parsed,
            text,
            rowsRead: Number(r.headers.get("x-d1-rows-read") ?? "0"),
            rowsWritten: Number(r.headers.get("x-d1-rows-written") ?? "0"),
            headers: r.headers,
        };
    };
    return {
        get: (p, headers) => call("GET", p, undefined, headers),
        post: (p, body, headers) => call("POST", p, body, headers),
    };
}

export function newHost() {
    return crypto.randomBytes(16).toString("hex");
}
export function sha256Hex(s) {
    return crypto.createHash("sha256").update(s).digest("hex");
}

// Result documents carry u64 counters above 2^53. These helpers keep every
// unquoted integer of 16+ digits as a tagged string through JSON.parse and
// restore it on output, independently of the server's parser.
const BIG = /(":\s*|\[\s*|,\s*)(-?\d{16,})(?=\s*[,}\]])/g;
export function parseExact(text) {
    return JSON.parse(text.replace(BIG, '$1"@@$2@@"'));
}
export function stringifyExact(v) {
    return JSON.stringify(v, null, 1).replace(/"@@(-?\d+)@@"/g, "$1");
}
const raw = (v) => (typeof v === "string" && v.startsWith("@@") ? v.slice(2, -2) : String(v));

/**
 * Independent JS port of checkpoint::result_digest_text, for documents parsed
 * with parseExact. The first line is the document's result_digest_schema, so a
 * test can build a consistent document of another schema.
 */
export function digestTextJs(d) {
    const p = d.params;
    const r = d.result;
    const pre = d.preflight;
    const known = (v) => (v === null ? "?" : raw(v));
    const lines = [
        ["block_h", p.block_h],
        ["K", raw(p.N)],
        ["assumes_below", p.assumes_below],
        ["case", `${raw(p.case_c)}/${raw(p.case_k)}`],
        ["A", raw(p.A)],
        ["B", raw(p.B)],
        ["bitvectors", raw(p.bitvectors)],
        ["kernel", p.kernel],
        ["mod3t", raw(p.mod3t)],
        ["partial_oee", p.partial_oee ? "1" : "0"],
        ["window_chains", known(p.window_chains)],
        ["window_templates_D", known(p.window_templates_D)],
        ["unwalked_depth", known(p.unwalked_depth)],
        ["record_threshold", p.record_threshold === null ? "-" : p.record_threshold],
        ["candidates_checked", raw(r.candidates_checked)],
        ["step2_masked_out", raw(r.step2_masked_out)],
        ["step1_survivors", raw(r.step1_survivors)],
        ["step3_steps_total", raw(r.step3_steps_total)],
        ["blocks_total", raw(r.blocks_total)],
        ["coverage_checksum", raw(r.coverage_checksum)],
        ["pow_checksum", raw(r.pow_checksum)],
        ["pow_kernel_id", raw(r.pow_kernel_id)],
        ["max_excursion", r.max_excursion],
        ["max_excursion_n", r.max_excursion_n],
        ["max_exceeds_u128", r.max_exceeds_u128 ? "1" : "0"],
        ["max_exact", r.max_exact === true ? "1" : r.max_exact === false && p.unwalked_depth !== null ? "0" : "?"],
        ["overflow_fallbacks", raw(r.overflow_fallbacks)],
        [
            "preflight",
            [pre.candidates, pre.coverage_checksum, pre.pow_checksum, pre.blocks_total, pre.step3_steps_total]
                .map(raw)
                .join(","),
        ],
        ["records", r.records.map(([n, peak]) => `${n}:${peak}`).join(";")],
        ["records_truncated", r.records_truncated ? "1" : "0"],
    ];
    return `${r.result_digest_schema}\n` + lines.map(([k, v]) => `${k}=${v}\n`).join("");
}

export function redigest(d) {
    d.result.result_digest = sha256Hex(digestTextJs(d));
    return d;
}

/** Fixture document moved to block h (fields that name the block, and the argmax offset). */
export function moveToBlock(d, h, K) {
    const w = 1n << BigInt(K);
    const oldL = BigInt(d.block.L);
    const L = BigInt(h) * w;
    d.block.h = String(h);
    d.block.L = L.toString();
    d.block.end = (L + w).toString();
    d.params.block_h = String(h);
    d.params.assumes_below = L.toString();
    d.result.max_excursion_n = (BigInt(d.result.max_excursion_n) - oldL + L).toString();
    d.result.records = d.result.records.map(([n, peak]) => [(BigInt(n) - oldL + L).toString(), peak]);
    return redigest(d);
}

/**
 * Fixture document moved to block h of 2^K numbers: the block fields, N and
 * unwalked_depth K - A + B follow, and the argmax and records keep their
 * offset from the block start (they stay inside it for K >= the fixture's K).
 */
export function toBlock(d, h, K) {
    d.block.K = K;
    d.params.N = K;
    d.params.unwalked_depth = K - Number(raw(d.params.A)) + Number(raw(d.params.B));
    return moveToBlock(d, h, K);
}

/** Fixture `text` moved to the lease's block (its h and size), edited, with a matching digest. */
export function resultForLease(text, lease, edit = () => {}) {
    const d = toBlock(parseExact(text), BigInt(lease.h), lease.k);
    edit(d);
    return stringifyExact(redigest(d));
}

/** The fields and value types of a lease, so a test can check that a re-check lease looks like any other. */
export const leaseShape = (l) =>
    Object.keys(l)
        .sort()
        .map((k) => `${k}:${l[k] === null ? "null" : Array.isArray(l[k]) ? "array" : typeof l[k]}`);

export function readFixture(name) {
    return fs.readFileSync(path.join(SERVER_DIR, "test", "fixtures", name), "utf8");
}
