// The public ledger (schema/0004_ledger.sql): one entry for every block that
// is accepted, withdrawn from the frontier or re-checked with the same result,
// each hash-chained to the one before it. An entry goes into the same D1 batch
// as the change it records, and its insert takes effect only while the
// ledger's head is the one the request read, so no block is accepted without
// its entry and no two entries share a seq: the request that loses reads
// again and retries, as for the units.rev guard. GET /api/ledger,
// /api/ledger.csv and /api/ledger/head publish the entries. Entries use the
// canonical line collatz-ledger/2, which also names who ran the result
// ('owner') and its votes (schema/0005_trust.sql); entries written before
// migration 0005 keep collatz-ledger/1, and every entry gives its format.

import { Db } from "./http";
import { JsonNumber, JsonValue, isObject, parseJson } from "./json";
import { Block, Campaign, blockRange } from "./result";
import { GUARD } from "./units";
import { LEDGER_FORMAT, LedgerEntry, LedgerFields, chainEntries, readHead } from "./ledger_chain";

export type { LedgerFields } from "./ledger_chain";
export { ledger, ledgerSummary } from "./ledger_publish";

/**
 * The statements that append `entries` (chainEntries on a head read in this
 * request) to a batch. Each insert takes effect only while the head is the
 * entry before it, by seq and hash, and the GUARD after it fails the batch
 * otherwise, so that the caller reads again and retries. `stale` (test mode)
 * makes the first insert miss, as when another request appended first.
 */
function appendStatements(db: Db, entries: LedgerEntry[], stale = false): D1PreparedStatement[] {
    return entries.flatMap((e, i) => [
        db.prep(
            `INSERT INTO ledger (seq, campaign_id, at, kind, start, bits, "end", result, peak, peak_n, seconds, machine, backend,
                                 version, schema, prev, hash, format, who, votes)
             SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?19, ?20, ?21
             WHERE EXISTS (SELECT 1 FROM ledger_head WHERE id = 1 AND seq = ?18 AND hash = ?16)`,
            e.seq,
            e.campaign,
            e.at,
            e.kind,
            e.start,
            e.bits,
            e.end,
            e.result,
            e.peak,
            e.peak_n,
            e.seconds,
            e.machine,
            e.backend,
            e.version,
            e.schema,
            e.prev,
            e.hash,
            stale && i === 0 ? -1 : e.seq - 1,
            LEDGER_FORMAT,
            e.who,
            e.votes,
        ),
        db.prep(GUARD),
    ]);
}

/**
 * Appends the entries `fields` to the batch `stmts`, numbered after the head
 * read here. The head is read last, just before the batch, so that the window
 * in which another request can append first stays short.
 */
export async function pushLedger(db: Db, stmts: D1PreparedStatement[], fields: LedgerFields[], stale = false): Promise<void> {
    if (fields.length > 0) {
        stmts.push(...appendStatements(db, await chainEntries(await readHead(db), fields), stale));
    }
}

function blockOf(c: Campaign, b: Block): { campaign: number; start: string; bits: number; end: string; schema: string } {
    const { start, end } = blockRange(c, b);
    return { campaign: c.id, start: start.toString(), bits: b.bits, end: end.toString(), schema: c.digest_schema };
}

const textAt = (o: JsonValue | undefined, key: string): string => {
    const v = isObject(o) ? o[key] : undefined;
    return typeof v === "string" ? v : "";
};

/**
 * A 'block' or 'recheck' entry for block `b` of `c`: the result with digest
 * `digest` that ran on the owner's machine `run` (the machine label is the
 * first 8 characters of its host id), from its stored document `doc`
 * (result/check.ts canonicalDocument), with `votes` votes. The backend is the
 * result's params.gpu, as on runs; the document's free-text backend label
 * stays out. Without the document (archived by scripts/prune.sql) the peak,
 * seconds and version are ''.
 */
export function resultFields(
    kind: "block" | "recheck",
    c: Campaign,
    b: Block,
    at: number,
    digest: string,
    doc: string | null,
    run: { host: string; backend: string },
    votes: number,
): LedgerFields {
    const out: LedgerFields = {
        ...blockOf(c, b),
        at,
        kind,
        result: digest,
        peak: "",
        peak_n: "",
        seconds: "",
        machine: run.host.slice(0, 8),
        backend: run.backend,
        version: "",
        who: "owner",
        votes,
    };
    if (doc !== null) {
        const d = parseJson(doc);
        const r = isObject(d) ? d["result"] : undefined;
        const p = isObject(d) ? d["params"] : undefined;
        const seconds = isObject(r) ? r["seconds_total"] : undefined;
        out.peak = textAt(r, "max_excursion");
        out.peak_n = textAt(r, "max_excursion_n");
        out.seconds = seconds instanceof JsonNumber ? seconds.raw : "";
        out.version = textAt(d, "commit");
        if (isObject(p) && typeof p["gpu"] === "boolean") {
            out.backend = p["gpu"] ? "gpu" : "cpu";
        }
    }
    return out;
}

/** A 'withdrawn' entry: block `b` of `c`, accepted with `digest`, stops counting. */
export function withdrawnFields(c: Campaign, b: Block, at: number, digest: string): LedgerFields {
    return {
        ...blockOf(c, b),
        at,
        kind: "withdrawn",
        result: digest,
        peak: "",
        peak_n: "",
        seconds: "",
        machine: "",
        backend: "",
        version: "",
        who: "",
        votes: 0,
    };
}
