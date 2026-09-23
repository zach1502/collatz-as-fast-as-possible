// Ledger entries and their hash chain (ledger.ts describes the ledger): the
// entry fields, the canonical line, numbering and linking after the head, and
// reading the head.

import { Db, HttpError } from "./http";
import { sha256Hex } from "./result";

/** The first field of the canonical line this server writes; each line format has its own name. */
export const LEDGER_FORMAT = "collatz-ledger/2";
/** The head of the empty ledger, and the prev of entry 1. */
export const LEDGER_GENESIS = "0".repeat(64);
/**
 * The fields of the canonical line after the format, in order: collatz-ledger/1
 * (entries written before migration 0005) and collatz-ledger/2, which adds who
 * and votes before prev.
 */
const LINE_FIELDS_V1 = [
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
] as const;
export const LINE_FIELDS = [...LINE_FIELDS_V1.slice(0, -1), "who", "votes", "prev"] as const;

type LedgerKind = "block" | "recheck" | "withdrawn";

/** An entry before it is numbered and linked. A text field the server does not know is ''. */
export interface LedgerFields {
    campaign: number;
    /** Unix seconds of the change. */
    at: number;
    kind: LedgerKind;
    /** The block [start, end) of 2^bits numbers, in decimal. */
    start: string;
    bits: number;
    end: string;
    /** The result digest the entry is about. */
    result: string;
    peak: string;
    peak_n: string;
    /** seconds_total as the result gave it. */
    seconds: string;
    machine: string;
    backend: string;
    version: string;
    schema: string;
    /** Who ran the result: 'owner' (the owner's machines); '' for a withdrawn block. */
    who: string;
    /** Votes for the result when the entry was made (units.checks); 0 for a withdrawn block. */
    votes: number;
}

export interface LedgerEntry extends LedgerFields {
    seq: number;
    prev: string;
    hash: string;
}

interface LedgerHead {
    seq: number;
    hash: string;
    at: number;
}

/**
 * The text whose SHA-256 is an entry's hash: LEDGER_FORMAT and the
 * LINE_FIELDS joined by "|" with no line end, numbers in decimal and text as
 * stored. Every field must be printable ASCII without "|", so that one line
 * stands for one entry only.
 */
function ledgerLine(e: LedgerFields & { seq: number; prev: string }): string {
    const values: string[] = [];
    for (const k of LINE_FIELDS) {
        const v = String(e[k]);
        if (!/^[ -~]*$/.test(v) || v.includes("|")) {
            throw new HttpError(500, `ledger field ${k} is not printable ASCII without "|"`);
        }
        values.push(v);
    }
    return [LEDGER_FORMAT, ...values].join("|");
}

/** Numbers `fields` as the entries after `head`, each linked to the one before it. */
export async function chainEntries(head: LedgerHead, fields: LedgerFields[]): Promise<LedgerEntry[]> {
    const out: LedgerEntry[] = [];
    let seq = head.seq;
    let prev = head.hash;
    for (const f of fields) {
        seq++;
        const hash = await sha256Hex(ledgerLine({ ...f, seq, prev }));
        out.push({ ...f, seq, prev, hash });
        prev = hash;
    }
    return out;
}

export async function readHead(db: Db): Promise<LedgerHead> {
    const h = await db.first<LedgerHead>("SELECT seq, hash, at FROM ledger_head WHERE id = 1");
    if (h === null) {
        throw new HttpError(500, "the ledger has no head row; apply migrations/0004_ledger.sql");
    }
    return h;
}
