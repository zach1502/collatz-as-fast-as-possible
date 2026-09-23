#!/usr/bin/env node
// Writes the SQL that creates a campaign, to be applied with
//   npx wrangler d1 execute collatz-frontier --remote --file campaign.sql
//
//   node scripts/campaign.mjs --name NAME --commit SHA[,SHA...] [--L0 DEC]
//       [--min-bits 44] [--max-bits 54] [--A 12] [--B 32] [--recheck-rate 0]
//       [--record-threshold DEC|none] [--assumption-source TEXT|none]
//       [--lease-hours 12] [--max-open 1000] [--end DEC] [--allow-dirty]
//       [--out campaign.sql]
//
// Each SHA is the `commit` field of a binary's `verify --json` output. A binary
// reports <sha>-dirty when its tree differed from the commit, <sha>-nogit when
// git could not check the tree and unknown outside a git checkout; such a commit
// does not say which code ran, so it is refused unless --allow-dirty is given.
//
// Workers choose the size of their blocks (`collatz worker --block-bits`)
// between 2^min-bits and 2^max-bits numbers; a block of 2^k numbers starts at a
// multiple of 2^k and is `collatz verify --block H --K k`. L0 must be a multiple
// of 2^max-bits. By default a campaign starts at Barina's frontier, 2075*2^60.
// One result accepts a block (quorum 1); --recheck-rate is the share of
// accepted blocks that the owner's machines run once more (default 0).

import fs from "node:fs";
import { pathToFileURL } from "node:url";

export const BARINA_L0 = 2075n << 60n;
/** The result digest schema campaigns take; must equal DIGEST_SCHEMA in src/result/digest.ts. */
const DIGEST_SCHEMA = "collatz-result-digest/2";
/** T-map peak of Barina's path record #97 (verify::kBarinaRecord97PeakT). */
export const RECORD97_PEAK_T = "494114680693632162318569565806744468898092";
export const DEFAULT_ASSUMPTION =
    "Barina live frontier (all n below 2075 times 2^60, pcbarina.fit.vutbr.cz, 2026-10-08) " +
    "and the earlier units of this campaign";

const DECIMAL = /^(0|[1-9][0-9]*)$/;
const COMMIT = /^[0-9A-Za-z._-]{1,80}$/;
/** The commits of a binary not built from a clean checkout (cmake/WriteGitCommitStamp.cmake). */
const UNCLEAN_COMMIT = /-(dirty|nogit)$|^unknown$/;
// The worker passes the source as one argv element, so it is limited to
// characters that no shell, cmd.exe or MinGW argv globbing treats specially.
const ASSUMPTION = /^[A-Za-z0-9 .,:;()'+=^/_-]{1,200}$/;
/** step2::max_A; the bit vectors of A = 14 are the widest the verifier builds. */
const MAX_A = 14;
/** step2::max_B; a build at B = 32 needs the 8 GiB dip table. */
const MAX_B = 32;

const sql = (v) =>
    v === null || v === undefined ? "NULL" : typeof v === "number" ? String(v) : `'${String(v).replaceAll("'", "''")}'`;

/**
 * Validates the options and returns one INSERT statement. Throws Error with a
 * message on bad input.
 */
export function campaignSql(o) {
    const minBits = o.minBits ?? 44;
    const maxBits = o.maxBits ?? 54;
    const A = o.A ?? 12;
    const B = o.B ?? 32;
    const L0 = BigInt(o.L0 ?? BARINA_L0);
    const name = o.name;
    if (typeof name !== "string" || !/^[A-Za-z0-9 _.-]{1,64}$/.test(name)) {
        throw new Error("--name must be 1-64 characters of letters, digits, space, _ . -");
    }
    for (const [k, v] of Object.entries({ "min-bits": minBits, "max-bits": maxBits, A, B })) {
        if (!Number.isInteger(v)) {
            throw new Error(`--${k} must be an integer`);
        }
    }
    if (minBits < 32 || maxBits > 64 || minBits > maxBits || maxBits - minBits > 30) {
        throw new Error("need 32 <= --min-bits <= --max-bits <= 64 and --max-bits - --min-bits <= 30");
    }
    // verify block mode: 0 < A < K and K - A <= 76 for every block size K, the
    // Step-2 limits on A and B.
    if (A < 1 || A > MAX_A || A >= minBits || maxBits - A > 76) {
        throw new Error(`--A must be 1..${MAX_A}, below --min-bits, and --max-bits - A <= 76`);
    }
    if (B < 2 || B > MAX_B) {
        throw new Error(`--B must be 2..${MAX_B}`);
    }
    const wMax = 1n << BigInt(maxBits);
    if (L0 <= 0n || L0 % wMax !== 0n || L0 < 1n << 17n) {
        throw new Error(`--L0 must be a positive multiple of 2^${maxBits} (2^max-bits, so that blocks of every size are aligned) and at least 2^17`);
    }
    // verify::validate_block on the first block of every size: (H+1)*2^K <= 2^96
    // and (H+1)*2^K*3^M < 2^(125+M) with M = K - A. Both bounds grow with the
    // block's end, so with --end the last block of every size (which ends at
    // --end) is checked too; the server hands out no block past them in an
    // open-ended campaign (src/alloc.ts verifierEnd).
    const outside = (top, k) => top > 1n << 96n || top * 3n ** BigInt(k - A) >= 1n << (125n + BigInt(k - A));
    for (let k = minBits; k <= maxBits; k++) {
        if (outside(L0 + (1n << BigInt(k)), k)) {
            throw new Error(`the first block of 2^${k} numbers is outside the verifier's block range (raise --A or lower --L0/--max-bits)`);
        }
    }
    let endPos = null;
    if (o.end !== undefined && o.end !== null) {
        const end = BigInt(o.end);
        if (end <= L0 || end % wMax !== 0n || end > 1n << 96n) {
            throw new Error(`--end must be above --L0, a multiple of 2^${maxBits} and at most 2^96`);
        }
        for (let k = minBits; k <= maxBits; k++) {
            if (outside(end, k)) {
                throw new Error(`the last block of 2^${k} numbers, which ends at --end, is outside the verifier's block range (raise --A or lower --end/--max-bits)`);
            }
        }
        endPos = Number((end - L0) >> BigInt(minBits));
    }
    const commits = (o.commits ?? []).filter((c) => c !== "");
    if (commits.length === 0 || !commits.every((c) => COMMIT.test(c))) {
        throw new Error("--commit needs one or more binary commits (comma-separated git SHAs)");
    }
    let threshold = o.recordThreshold;
    if (threshold === undefined) {
        threshold = L0 >= 1n << 71n ? RECORD97_PEAK_T : null;
    } else if (threshold === "none") {
        threshold = null;
    } else if (!DECIMAL.test(threshold)) {
        throw new Error("--record-threshold must be a decimal integer or none");
    }
    let source = o.assumptionSource;
    if (source === undefined) {
        source = L0 === BARINA_L0 ? DEFAULT_ASSUMPTION : null;
    } else if (source === "none") {
        source = null;
    } else if (!ASSUMPTION.test(source)) {
        throw new Error("--assumption-source: up to 200 of A-Z a-z 0-9 space . , : ; ( ) ' + = ^ / _ -");
    }
    const recheckRate = o.recheckRate ?? 0;
    // Default 12 hours. A 2^54 block takes about 16 minutes on an RTX 3070, and
    // a stopped machine's lease holds its unit back for at most the lease time.
    const lease = o.leaseSeconds ?? 43200;
    const maxOpen = o.maxOpen ?? 1000;
    if (typeof recheckRate !== "number" || !Number.isFinite(recheckRate) || recheckRate < 0 || recheckRate > 1) {
        throw new Error("--recheck-rate must be a number from 0 to 1");
    }
    if (!Number.isInteger(lease) || lease < 1 || !Number.isInteger(maxOpen) || maxOpen < 1) {
        throw new Error("--lease-hours and --max-open must be positive");
    }
    const cols = {
        id: o.id ?? null,
        name,
        active: 1,
        l0: L0.toString(),
        min_bits: minBits,
        max_bits: maxBits,
        a: A,
        b: B,
        kernel: "table16",
        bitvectors: 8,
        mod3t: 6,
        partial_oee: 1,
        window_chains: 6,
        window_templates_d: 12,
        record_threshold: threshold,
        assumption_source: source,
        commits: JSON.stringify(commits),
        digest_schema: DIGEST_SCHEMA,
        // One result accepts a block (requireRunnable in src/units/campaign.ts).
        quorum: 1,
        recheck_rate: recheckRate,
        lease_seconds: lease,
        max_open: maxOpen,
        end_pos: endPos,
        created_at: Math.floor(Date.now() / 1000),
    };
    if (cols.id === null) {
        delete cols.id;
    }
    return `INSERT INTO campaigns (${Object.keys(cols).join(", ")}) VALUES (${Object.values(cols).map(sql).join(", ")});\n`;
}

/**
 * Throws unless every commit names a clean checkout. The command line calls it
 * unless --allow-dirty is given; campaignSql takes any commit, so that tests can
 * set up campaigns for the commits their fixtures and test binaries report.
 */
function refuseUncleanCommits(commits) {
    const unclean = commits.filter((c) => UNCLEAN_COMMIT.test(c));
    if (unclean.length > 0) {
        throw new Error(
            `--commit ${unclean.join(",")}: a binary reports -dirty when its tree differed from the commit, ` +
                "-nogit when git could not check the tree and unknown outside a git checkout, so this does not " +
                "say which code ran; build from a clean checkout, or pass --allow-dirty to list it anyway",
        );
    }
}

function parseArgs(argv) {
    const o = {};
    let out = "campaign.sql";
    let allowDirty = false;
    const num = (s, flag) => {
        if (!/^[0-9]+$/.test(s ?? "")) {
            throw new Error(`${flag} needs a non-negative integer`);
        }
        return Number(s);
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const v = argv[i + 1];
        const take = () => {
            if (v === undefined) {
                throw new Error(`${a} needs a value`);
            }
            i++;
            return v;
        };
        switch (a) {
            case "--name":
                o.name = take();
                break;
            case "--commit":
                o.commits = take().split(",");
                break;
            case "--L0":
            case "--end": {
                const d = take();
                if (!DECIMAL.test(d)) {
                    throw new Error(`${a} must be a decimal integer`);
                }
                o[a === "--L0" ? "L0" : "end"] = d;
                break;
            }
            case "--min-bits":
                o.minBits = num(take(), a);
                break;
            case "--max-bits":
                o.maxBits = num(take(), a);
                break;
            case "--A":
                o.A = num(take(), a);
                break;
            case "--B":
                o.B = num(take(), a);
                break;
            case "--record-threshold":
                o.recordThreshold = take();
                break;
            case "--assumption-source":
                o.assumptionSource = take();
                break;
            case "--recheck-rate": {
                const r = take();
                if (!/^(0|1|0?\.[0-9]+|1\.0*)$/.test(r)) {
                    throw new Error("--recheck-rate needs a number from 0 to 1");
                }
                o.recheckRate = Number(r);
                break;
            }
            case "--lease-hours":
                o.leaseSeconds = Math.round(Number(take()) * 3600);
                break;
            case "--max-open":
                o.maxOpen = num(take(), a);
                break;
            case "--out":
                out = take();
                break;
            case "--allow-dirty":
                allowDirty = true;
                break;
            default:
                throw new Error(`unknown argument ${a}`);
        }
    }
    return { o, out, allowDirty };
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
    try {
        const { o, out, allowDirty } = parseArgs(process.argv.slice(2));
        if (!allowDirty) {
            refuseUncleanCommits(o.commits ?? []);
        }
        const text = campaignSql(o);
        fs.writeFileSync(out, text);
        process.stdout.write(text);
        process.stderr.write(
            `wrote ${out}; apply it with\n  npx wrangler d1 execute collatz-frontier --remote --file ${out}\n`,
        );
    } catch (e) {
        process.stderr.write(`error: ${e.message}\n`);
        process.exit(2);
    }
}
