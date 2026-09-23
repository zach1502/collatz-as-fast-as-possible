#!/usr/bin/env node
// Writes public/path-records.json, the known path records the status page
// lists, from data/path-records.csv (sources in data/README.md). `npm test`
// checks the two are in step.
//     node scripts/build-path-records.mjs          write public/path-records.json
//     node scripts/build-path-records.mjs --check  exit 1 if it is stale

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
export const CSV_PATH = path.join(root, "data", "path-records.csv");
export const JSON_PATH = path.join(root, "public", "path-records.json");
const HEADER = "number,n,peak,discoverer,source";
const DECIMAL = /^[1-9][0-9]*$/;

/** The largest value on the orbit of n under the 3n+1 map, down to 1. */
export function peakOf(n) {
    let top = n;
    for (let x = n; x !== 1n; ) {
        x = x % 2n === 1n ? 3n * x + 1n : x / 2n;
        if (x > top) {
            top = x;
        }
    }
    return top;
}

/**
 * The JSON text for `csv`: one {number, n, peak, discoverer} per line, n and
 * peak as decimal strings, discoverer null where the CSV has none. Throws an
 * Error naming the line unless the rows are numbered 1, 2, 3, ..., n and peak
 * both increase, and each peak is the largest value on the orbit of its n.
 */
export function buildPathRecords(csv) {
    const lines = csv.split(/\r?\n/).filter((l) => l !== "");
    if (lines[0] !== HEADER) {
        throw new Error(`line 1 must be ${HEADER}`);
    }
    const rows = [];
    let prev = { n: 0n, peak: 0n };
    for (const [i, line] of lines.slice(1).entries()) {
        const fail = (why) => {
            throw new Error(`line ${i + 2}: ${why}`);
        };
        const f = line.split(",");
        if (f.length !== 5) {
            fail("needs 5 fields");
        }
        const [number, n, peak, discoverer, source] = f;
        if (number !== String(i + 1)) {
            fail(`number ${number}, expected ${i + 1}`);
        }
        if (!DECIMAL.test(n) || !DECIMAL.test(peak)) {
            fail("n and peak must be decimal integers");
        }
        const cur = { n: BigInt(n), peak: BigInt(peak) };
        if (cur.n <= prev.n || cur.peak <= prev.peak) {
            fail("n and peak must both exceed those of the record before");
        }
        const top = peakOf(cur.n);
        if (top !== cur.peak) {
            fail(`the orbit of ${n} peaks at ${top}, not ${peak}`);
        }
        if (!/^[ -~]{0,80}$/.test(discoverer) || !/^[A-Za-z0-9]+(;[A-Za-z0-9]+)*$/.test(source)) {
            fail("discoverer must be printable ASCII and source a ;-separated list of names");
        }
        rows.push({ number: i + 1, n, peak, discoverer: discoverer === "" ? null : discoverer });
        prev = cur;
    }
    return `[\n${rows.map((r) => JSON.stringify(r)).join(",\n")}\n]\n`;
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
    try {
        const text = buildPathRecords(fs.readFileSync(CSV_PATH, "utf8"));
        if (process.argv.includes("--check")) {
            // A checkout with CRLF line ends holds the same file.
            const current = fs.existsSync(JSON_PATH) ? fs.readFileSync(JSON_PATH, "utf8").replaceAll("\r\n", "\n") : null;
            if (current !== text) {
                console.error("public/path-records.json is out of date: run node scripts/build-path-records.mjs");
                process.exit(1);
            }
        } else {
            fs.writeFileSync(JSON_PATH, text);
        }
    } catch (e) {
        console.error(`error: data/path-records.csv ${e.message}`);
        process.exit(2);
    }
}
