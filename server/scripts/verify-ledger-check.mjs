// The checks of scripts/verify-ledger.mjs: each entry's canonical line and
// hash, the chain of prev hashes and seqs, the blocks that count per campaign,
// and anchor lines.

import crypto from "node:crypto";

/** The canonical line formats (src/ledger_chain.ts): /1 for entries written before migration 0005, /2 for the rest. */
const V1 = "collatz-ledger/1";
const V2 = "collatz-ledger/2";
/** The prev of entry 1. */
export const GENESIS = "0".repeat(64);
/** The fields of a collatz-ledger/1 line after the format, in order. */
export const LINE_FIELDS_V1 = [
    "seq",
    "campaign",
    "at",
    "kind",
    "start",
    "bits",
    "end",
    "result",
    "peak",
    "peak_n",
    "seconds",
    "machine",
    "backend",
    "version",
    "schema",
    "prev",
];
/** collatz-ledger/2 adds who ran the result and its votes before prev. */
export const LINE_FIELDS_V2 = [...LINE_FIELDS_V1.slice(0, -1), "who", "votes", "prev"];
const LINE_FIELDS = { [V1]: LINE_FIELDS_V1, [V2]: LINE_FIELDS_V2 };
const DECIMAL = /^(0|[1-9][0-9]*)$/;
const KINDS = new Set(["block", "recheck", "withdrawn"]);
/** Who ran the result of a collatz-ledger/2 entry: the owner's machines; empty for a withdrawn block. */
const WHO_V2 = new Set(["owner", ""]);
const ANCHOR = /^collatz-ledger head ([0-9]+) ([0-9a-f]{64})(?:\s|$)/;

/** A ledger that does not verify; `seq` is the first entry that fails. */
export class LedgerError extends Error {
    constructor(seq, message) {
        super(message);
        this.seq = seq;
    }
}

const sha256Hex = (s) => crypto.createHash("sha256").update(s).digest("hex");

export const isoTime = (at) => new Date(Number(at) * 1000).toISOString().replace(/\.000Z$/, "Z");

/**
 * The format of an entry: collatz-ledger/1 when it names none, since copies
 * saved from a server without migration 0005 have no format field.
 */
const formatOf = (e) => (e.format === undefined ? V1 : e.format);

/**
 * The canonical line of an entry (src/ledger_chain.ts ledgerLine), or null
 * when its format is unknown or a field is not printable ASCII without "|".
 */
function ledgerLine(e) {
    const format = formatOf(e);
    const fields = Object.hasOwn(LINE_FIELDS, format) ? LINE_FIELDS[format] : null;
    if (fields === null) {
        return null;
    }
    const values = fields.map((k) => String(e[k]));
    return values.every((v) => /^[ -~]*$/.test(v) && !v.includes("|")) ? [format, ...values].join("|") : null;
}

/** Index of the first block of `list` (sorted by start) that starts at or after `start`. */
function lowerBound(list, start) {
    let lo = 0;
    let hi = list.length;
    while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (list[mid].start < start) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * Checks `entries` in the order given and returns the blocks that count:
 * campaign id (as text) -> [{start, end, seq}] sorted by start, start and end
 * BigInt. A 'withdrawn' entry that matches no counting block is accepted: it
 * names a block accepted before the ledger existed. Throws LedgerError at the
 * first entry that fails.
 */
export function checkLedger(entries) {
    const counted = new Map();
    let prev = GENESIS;
    for (let i = 0; i < entries.length; i++) {
        const e = entries[i];
        const seq = i + 1;
        if (e === null || typeof e !== "object" || Array.isArray(e)) {
            throw new LedgerError(seq, "is not an object");
        }
        const format = formatOf(e);
        if (!Object.hasOwn(LINE_FIELDS, format)) {
            throw new LedgerError(seq, `has format ${JSON.stringify(String(format)).slice(0, 40)}, which this script does not know`);
        }
        const absent = [...LINE_FIELDS[format], "hash"].find((k) => !(k in e));
        if (absent !== undefined) {
            throw new LedgerError(seq, `has no field ${absent}`);
        }
        // A /1 line binds no who or votes, so an entry of that format must not claim any.
        if (format === V1 && ((e.who ?? "") !== "" || String(e.votes ?? 0) !== "0")) {
            throw new LedgerError(seq, "is a collatz-ledger/1 entry with a who or votes, which its hash does not cover");
        }
        if (format === V2 && (!WHO_V2.has(e.who) || !/^[0-9]{1,9}$/.test(String(e.votes)))) {
            throw new LedgerError(seq, "who or votes is malformed");
        }
        if (String(e.seq) !== String(seq)) {
            throw new LedgerError(seq, `the entry in its place has seq ${e.seq}; seq counts 1, 2, 3, ... without gaps`);
        }
        if (e.prev !== prev) {
            throw new LedgerError(seq, `prev is not the hash of ${seq === 1 ? "the empty ledger (64 zeros)" : `entry ${seq - 1}`}`);
        }
        const line = ledgerLine(e);
        if (line === null) {
            throw new LedgerError(seq, 'a field is not printable ASCII without "|"');
        }
        if (sha256Hex(line) !== e.hash) {
            throw new LedgerError(seq, "hash is not the SHA-256 of the entry's canonical line");
        }
        prev = e.hash;
        const [campaign, start, bits, end] = [e.campaign, e.start, e.bits, e.end].map(String);
        if (!KINDS.has(e.kind) || ![campaign, start, end].every((v) => DECIMAL.test(v)) || !/^[0-9]{1,3}$/.test(bits)) {
            throw new LedgerError(seq, "kind, campaign, start, bits or end is malformed");
        }
        const block = { start: BigInt(start), end: BigInt(end), seq };
        if (block.end - block.start !== 1n << BigInt(bits)) {
            throw new LedgerError(seq, "end is not start + 2^bits");
        }
        if (e.kind === "recheck") {
            continue;
        }
        const list = counted.get(campaign) ?? [];
        counted.set(campaign, list);
        const at = lowerBound(list, block.start);
        if (e.kind === "withdrawn") {
            if (at < list.length && list[at].start === block.start && list[at].end === block.end) {
                list.splice(at, 1);
            }
            continue;
        }
        const clash = at < list.length && list[at].start < block.end ? list[at] : at > 0 && list[at - 1].end > block.start ? list[at - 1] : null;
        if (clash !== null) {
            const what = `block [${start}, ${end}) of campaign ${campaign}`;
            throw new LedgerError(seq, `${what} overlaps the block of entry ${clash.seq}, which was not withdrawn`);
        }
        list.splice(at, 0, block);
    }
    return counted;
}

/** Per campaign: the blocks that count, the numbers in them, and the contiguous run from the lowest start. */
export function summarize(counted) {
    return [...counted]
        .filter(([, list]) => list.length > 0)
        .sort(([a], [b]) => Number(a) - Number(b))
        .map(([campaign, list]) => {
            let frontier = list[0].end;
            for (let i = 1; i < list.length && list[i].start === frontier; i++) {
                frontier = list[i].end;
            }
            const numbers = list.reduce((sum, b) => sum + (b.end - b.start), 0n);
            return { campaign, blocks: list.length, numbers, from: list[0].start, frontier };
        });
}

/** Checks every anchor line of `text` against the verified `entries`, lowest seq first; returns how many there were. */
export function checkAnchors(entries, text) {
    const anchors = [];
    for (const line of text.split(/\r?\n/)) {
        if (!line.startsWith("collatz-ledger head ")) {
            continue;
        }
        const m = ANCHOR.exec(line);
        if (m === null || !DECIMAL.test(m[1]) || m[1] === "0") {
            throw new Error(`not an anchor line: ${line}`);
        }
        anchors.push({ seq: Number(m[1]), hash: m[2] });
    }
    for (const a of anchors.sort((x, y) => x.seq - y.seq)) {
        const e = entries[a.seq - 1];
        if (e === undefined) {
            throw new LedgerError(a.seq, `is anchored, but the ledger ends at seq ${entries.length}`);
        }
        if (e.hash !== a.hash) {
            throw new LedgerError(a.seq, `is anchored with hash ${a.hash}, the ledger has ${e.hash}`);
        }
    }
    return anchors.length;
}
