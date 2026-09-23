#!/usr/bin/env node
// Checks a frontier server's public ledger (README.md, "Ledger"), read
// page by page from the server's /api/ledger or from a saved /api/ledger JSON
// array or /api/ledger.csv file. Every hash must be the SHA-256 of its
// entry's canonical line (collatz-ledger/1, or /2 with who and votes, as the
// entry's format says), every prev the hash before it, seq must count 1, 2,
// 3, ... without gaps, and within a campaign the blocks that count (a 'block'
// entry with no later 'withdrawn' entry of the same block) must not overlap.
// Prints the head, the entry count and, per campaign, the numbers in the
// blocks that count and the frontier of their contiguous run from the lowest
// start. Exit 1 names the first bad seq; exit 2 is a usage or read error.
// Node.js 20.3 or newer, no dependencies.
//
//   node scripts/verify-ledger.mjs https://SERVER [--anchors FILE]
//   node scripts/verify-ledger.mjs ledger.json|ledger.csv [--anchors FILE]
//
// --anchors FILE: every "collatz-ledger head SEQ HASH ..." line of FILE, with
// SEQ and HASH as /api/ledger/head gives them, must name an entry of the
// ledger and its hash.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { GENESIS, LedgerError, checkAnchors, checkLedger, isoTime, summarize } from "./verify-ledger-check.mjs";
import { parseSaved, readServer } from "./verify-ledger-read.mjs";

function usage(msg) {
    process.stderr.write(
        `error: ${msg}\n` +
            "usage: node scripts/verify-ledger.mjs (https://SERVER | ledger.json | ledger.csv) [--anchors FILE]\n",
    );
    process.exit(2);
}

async function main(argv) {
    let source = null;
    let anchorFile = null;
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === "--anchors") {
            anchorFile = argv[++i] ?? usage("--anchors needs a file");
        } else if (argv[i].startsWith("-") || source !== null) {
            usage(`unknown argument ${argv[i]}`);
        } else {
            source = argv[i];
        }
    }
    if (source === null) {
        usage("give a server URL or a saved ledger file");
    }
    const anchorText = anchorFile === null ? null : fs.readFileSync(anchorFile, "utf8");
    const remote = /^https?:\/\//.test(source);
    const base = source.replace(/\/+$/, "");
    const { head, entries } = remote ? await readServer(base) : { head: null, entries: parseSaved(fs.readFileSync(source, "utf8")) };
    try {
        const counted = checkLedger(entries);
        const last = entries[entries.length - 1];
        if (head !== null && head.seq > 0 && head.hash !== last.hash) {
            throw new LedgerError(head.seq, `${base}/api/ledger/head gives hash ${head.hash}, the entry has ${last.hash}`);
        }
        const anchors = anchorText === null ? null : checkAnchors(entries, anchorText);
        const out = [
            `source: ${remote ? base : path.resolve(source)}`,
            `entries: ${entries.length}`,
            `head: ${entries.length} ${last?.hash ?? GENESIS}${last === undefined ? "" : ` at ${isoTime(last.at)}`}`,
            ...summarize(counted).map(
                (c) => `campaign ${c.campaign}: blocks ${c.blocks}, numbers ${c.numbers}, frontier ${c.frontier} (contiguous from ${c.from})`,
            ),
            ...(anchors === null ? [] : [`anchors: ${anchors} checked`]),
            "ledger OK",
        ];
        process.stdout.write(out.map((l) => `${l}\n`).join(""));
        return 0;
    } catch (e) {
        if (e instanceof LedgerError) {
            process.stdout.write(`FAIL at seq ${e.seq}: ${e.message}\n`);
            return 1;
        }
        throw e;
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    main(process.argv.slice(2)).then(
        (code) => {
            process.exitCode = code;
        },
        (e) => {
            process.stderr.write(`error: ${e.message}\n`);
            process.exitCode = 2;
        },
    );
}
