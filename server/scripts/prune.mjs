#!/usr/bin/env node
// Backs up the D1 database and then runs scripts/prune.sql on it, which frees
// the space of data the server no longer needs (that file says what it keeps).
// D1 grows by about 2.7 KB per accepted block, about half of it the block's
// result document (unit_results), so at the 1500 blocks a day of one RTX 3070
// on 2^50 blocks the free plan's 500 MB last about four months unpruned.
//
//   node scripts/prune.mjs [--database collatz-frontier] [--days 30]
//       [--backup FILE | --no-backup] [-- WRANGLER_ARGS...]
//
// --days is the age after acceptance at which a block's documents are
// archived (default 30). The backup (`wrangler d1 export`, default
// backup-YYYY-MM-DD.sql in server/) holds them. Arguments after -- replace
// --remote in the wrangler commands, for example to prune a local database.

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
/** The age in prune.sql, in seconds; the script replaces it with --days. */
const DEFAULT_AGE = "2592000";

function usage(msg) {
    process.stderr.write(
        `error: ${msg}\nusage: node scripts/prune.mjs [--database NAME] [--days N] [--backup FILE | --no-backup] [-- WRANGLER_ARGS...]\n`,
    );
    process.exit(2);
}

export function parseArgs(argv) {
    const o = { database: "collatz-frontier", days: 30, backup: `backup-${new Date().toISOString().slice(0, 10)}.sql`, target: ["--remote"] };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const value = () => (argv[i + 1] === undefined ? usage(`${a} needs a value`) : argv[++i]);
        if (a === "--") {
            o.target = argv.slice(i + 1);
            break;
        } else if (a === "--database") {
            o.database = value();
        } else if (a === "--days") {
            const v = value();
            if (!/^[0-9]{1,5}$/.test(v)) {
                usage("--days needs a whole number of days");
            }
            o.days = Number(v);
        } else if (a === "--backup") {
            o.backup = value();
        } else if (a === "--no-backup") {
            o.backup = null;
        } else {
            usage(`unknown argument ${a}`);
        }
    }
    return o;
}

/** Runs `npx wrangler ARGS` in server/; throws with its output when it fails. */
function wrangler(args) {
    // npx is a .cmd file on Windows, which only runs through a shell.
    const quoted = args.map((a) => (/[\s"]/.test(a) ? `"${a.replace(/"/g, '\\"')}"` : a));
    const r = spawnSync("npx", ["wrangler", ...quoted], { cwd: root, shell: true, encoding: "utf8" });
    if (r.status !== 0) {
        process.stdout.write(r.stdout ?? "");
        process.stderr.write(r.stderr ?? "");
        throw new Error(`wrangler ${args.slice(0, 3).join(" ")} failed (exit ${r.status})`);
    }
}

function main() {
    const o = parseArgs(process.argv.slice(2));
    if (o.backup !== null) {
        wrangler(["d1", "export", o.database, ...o.target, "--output", o.backup]);
        console.log(`backup: ${path.resolve(root, o.backup)}`);
    }
    const sql = fs.readFileSync(path.join(root, "scripts", "prune.sql"), "utf8").replaceAll(DEFAULT_AGE, String(o.days * 86400));
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "collatz-prune-"));
    try {
        const file = path.join(tmp, "prune.sql");
        fs.writeFileSync(file, sql);
        wrangler(["d1", "execute", o.database, ...o.target, "--yes", "--file", file]);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
    console.log(`pruned ${o.database}: documents of blocks accepted more than ${o.days} days ago are archived`);
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
    try {
        main();
    } catch (e) {
        process.stderr.write(`error: ${e.message}\n`);
        process.exit(1);
    }
}
