// Checks a result document against its unit and campaign, and rebuilds the
// stored form and the block_stats figures of an accepted one.

import { JsonNumber, JsonObject, JsonValue, isObject, parseJson, stringifyJson } from "../json";
import { Block, Campaign, allowedCommits, blockRange, expectedDepth } from "./campaign";
import { DIGEST_SCHEMA, digestText, sha256Hex } from "./digest";
import { RecordHit, ResultError, bool, decString, get, obj, plainNumber, plainText, records, text, u64, u64OrNull } from "./fields";

const HEX64 = /^[0-9a-f]{64}$/;

interface CheckedResult {
    digest: string;
    records: RecordHit[];
    /** The document to store: canonicalDocument() of the checked result. */
    canonical: string;
    /** The backend the result says it ran on (params.gpu); the digest does not bind it. */
    backend: "gpu" | "cpu";
    /** What block_stats keeps of the result (src/stats/block_stats.ts). */
    figures: BlockFigures;
}

/** The figures of a result that /api/stats shows per block (block_stats). */
export interface BlockFigures {
    /** result.seconds_total; the digest does not bind it. */
    seconds: number;
    /** The document's backend label ("gpu", "cpu", "cpu (fallback)", ...). */
    backend: string;
    candidates: string;
    /** result.blocks_total: Step-3 blocks walked. */
    walked: string;
    overflow: string;
    maxExcursion: string;
    maxExcursionN: string;
}

/** The figures of a document checkResult accepted, or of one canonicalDocument stored. */
function figuresOf(doc: JsonObject): BlockFigures {
    const r = obj(doc, "result", "");
    return {
        seconds: Number(plainNumber(r, "seconds_total", "result.")),
        backend: plainText(doc, "backend", "", 80),
        candidates: u64(r, "candidates_checked", "result."),
        walked: u64(r, "blocks_total", "result."),
        overflow: u64(r, "overflow_fallbacks", "result."),
        maxExcursion: decString(r, "max_excursion", "result."),
        maxExcursionN: decString(r, "max_excursion_n", "result."),
    };
}

/** The figures of a document stored by canonicalDocument (unit_results, runs.result_json). */
export function storedFigures(text: string): BlockFigures {
    const doc = parseJson(text);
    if (!isObject(doc)) {
        throw new ResultError("a stored result is not a JSON object");
    }
    return figuresOf(doc);
}

/**
 * The stored form of an accepted result, rebuilt from checked fields only:
 * every field the digest binds, the parameters checkResult compares with the
 * campaign, and four informational ones with a checked format (backend,
 * threads, gpu, seconds_total), in a fixed order. Unknown keys, the machine
 * block and the timings of the stages are dropped. u64 counters keep their
 * exact text. `doc` must be a document checkResult accepted.
 */
function canonicalDocument(doc: JsonObject): string {
    const block = obj(doc, "block", "");
    const p = obj(doc, "params", "");
    const r = obj(doc, "result", "");
    const pre = obj(doc, "preflight", "");
    const u = (o: JsonObject, key: string, path: string): JsonNumber => new JsonNumber(u64(o, key, path));
    const orNull = (o: JsonObject, key: string, path: string): JsonNumber | null => {
        const v = u64OrNull(o, key, path);
        return v === null ? null : new JsonNumber(v);
    };
    const threshold = get(p, "record_threshold", "params.");
    const out: JsonObject = {
        schema: text(doc, "schema", ""),
        commit: text(doc, "commit", ""),
        backend: plainText(doc, "backend", "", 80),
        block: {
            h: decString(block, "h", "block."),
            K: u(block, "K", "block."),
            L: decString(block, "L", "block."),
            end: decString(block, "end", "block."),
        },
        params: {
            N: u(p, "N", "params."),
            A: u(p, "A", "params."),
            B: u(p, "B", "params."),
            bitvectors: u(p, "bitvectors", "params."),
            kernel: text(p, "kernel", "params."),
            mod3t: u(p, "mod3t", "params."),
            partial_oee: bool(p, "partial_oee", "params."),
            block_h: decString(p, "block_h", "params."),
            window_chains: orNull(p, "window_chains", "params."),
            window_templates_D: orNull(p, "window_templates_D", "params."),
            assumes_below: decString(p, "assumes_below", "params."),
            assumption_source: plainText(p, "assumption_source", "params.", 200),
            record_threshold: threshold === null ? null : decString(p, "record_threshold", "params."),
            window_landing: bool(p, "window_landing", "params."),
            unwalked_depth: orNull(p, "unwalked_depth", "params."),
            case_c: u(p, "case_c", "params."),
            case_k: u(p, "case_k", "params."),
            threads: u(p, "threads", "params."),
            gpu: bool(p, "gpu", "params."),
        },
        result: {
            candidates_checked: u(r, "candidates_checked", "result."),
            step2_masked_out: u(r, "step2_masked_out", "result."),
            step1_survivors: u(r, "step1_survivors", "result."),
            step3_steps_total: u(r, "step3_steps_total", "result."),
            blocks_total: u(r, "blocks_total", "result."),
            coverage_checksum: u(r, "coverage_checksum", "result."),
            pow_checksum: u(r, "pow_checksum", "result."),
            pow_available: bool(r, "pow_available", "result."),
            pow_kernel_id: u(r, "pow_kernel_id", "result."),
            max_excursion: decString(r, "max_excursion", "result."),
            max_excursion_n: decString(r, "max_excursion_n", "result."),
            max_exceeds_u128: bool(r, "max_exceeds_u128", "result."),
            max_exact: bool(r, "max_exact", "result."),
            overflow_fallbacks: u(r, "overflow_fallbacks", "result."),
            records: records(r).map((h) => [h.n, h.peak]),
            records_truncated: bool(r, "records_truncated", "result."),
            seconds_total: new JsonNumber(plainNumber(r, "seconds_total", "result.")),
            result_digest_schema: text(r, "result_digest_schema", "result."),
            result_digest: text(r, "result_digest", "result."),
        },
        preflight: {
            candidates: u(pre, "candidates", "preflight."),
            coverage_checksum: u(pre, "coverage_checksum", "preflight."),
            pow_checksum: u(pre, "pow_checksum", "preflight."),
            blocks_total: u(pre, "blocks_total", "preflight."),
            step3_steps_total: u(pre, "step3_steps_total", "preflight."),
        },
    };
    return stringifyJson(out);
}

/**
 * Validates `doc` as the result of block `unit` of campaign `c`: every
 * parameter that enters the digest must equal the campaign's (K is the
 * block's own size), the commit must be allowed, the counters must be
 * canonical, and result_digest must equal the SHA-256 of the canonical text
 * rebuilt from the fields.
 */
export async function checkResult(doc: JsonValue, c: Campaign, unit: Block): Promise<CheckedResult> {
    if (!isObject(doc)) {
        throw new ResultError("result_json is not a JSON object");
    }
    if (doc["schema"] !== "collatz-verify-json/1") {
        throw new ResultError('schema is not "collatz-verify-json/1"');
    }
    if (c.digest_schema !== DIGEST_SCHEMA) {
        throw new ResultError(`campaign ${c.id} records digest schema ${c.digest_schema}; this server checks ${DIGEST_SCHEMA} only`);
    }
    const commit = text(doc, "commit", "");
    if (!allowedCommits(c).includes(commit)) {
        throw new ResultError(`binary commit ${commit} is not accepted by this campaign`);
    }
    const { h, start, end } = blockRange(c, unit);
    const block = obj(doc, "block", "");
    const p = obj(doc, "params", "");
    const r = obj(doc, "result", "");
    const pre = obj(doc, "preflight", "");
    const want = (ok: boolean, what: string): void => {
        if (!ok) {
            throw new ResultError(`${what} does not match the unit (block ${h} of 2^${unit.bits} numbers)`);
        }
    };
    want(decString(block, "h", "block.") === h.toString(), "block.h");
    want(u64(block, "K", "block.") === String(unit.bits), "block.K");
    want(decString(block, "L", "block.") === start.toString(), "block.L");
    want(decString(block, "end", "block.") === end.toString(), "block.end");
    want(u64(p, "N", "params.") === String(unit.bits), "params.N");
    want(decString(p, "block_h", "params.") === h.toString(), "params.block_h");
    want(decString(p, "assumes_below", "params.") === start.toString(), "params.assumes_below");
    want(u64(p, "A", "params.") === String(c.a), "params.A");
    want(u64(p, "B", "params.") === String(c.b), "params.B");
    want(u64(p, "bitvectors", "params.") === String(c.bitvectors), "params.bitvectors");
    want(text(p, "kernel", "params.") === c.kernel, "params.kernel");
    want(u64(p, "mod3t", "params.") === String(c.mod3t), "params.mod3t");
    want(bool(p, "partial_oee", "params.") === (c.partial_oee !== 0), "params.partial_oee");
    want(get(p, "window_chains", "params.") !== null && u64(p, "window_chains", "params.") === String(c.window_chains), "params.window_chains");
    want(
        get(p, "window_templates_D", "params.") !== null &&
            u64(p, "window_templates_D", "params.") === String(c.window_templates_d),
        "params.window_templates_D",
    );
    want(get(p, "unwalked_depth", "params.") !== null && u64(p, "unwalked_depth", "params.") === String(expectedDepth(c, unit.bits)), "params.unwalked_depth");
    want(get(p, "record_threshold", "params.") === c.record_threshold, "params.record_threshold");
    want(u64(p, "case_c", "params.") === "0" && u64(p, "case_k", "params.") === "1", "params.case_c/case_k");
    want(bool(p, "window_landing", "params.") === false, "params.window_landing");
    if (c.assumption_source !== null) {
        want(text(p, "assumption_source", "params.") === c.assumption_source, "params.assumption_source");
    }
    const schema = text(r, "result_digest_schema", "result.");
    if (schema !== c.digest_schema) {
        throw new ResultError(
            `result.result_digest_schema is ${JSON.stringify(schema).slice(0, 80)}; this campaign takes ${c.digest_schema} (build one of the allowed commits)`,
        );
    }
    if (!bool(r, "pow_available", "result.")) {
        throw new ResultError("result.pow_available is false");
    }
    if (typeof get(r, "max_exact", "result.") !== "boolean") {
        throw new ResultError("result.max_exact is not a boolean");
    }
    for (const key of ["candidates", "coverage_checksum", "pow_checksum", "blocks_total", "step3_steps_total"]) {
        if (u64(pre, key, "preflight.") !== "0") {
            throw new ResultError(`preflight.${key} is not 0 (block runs have no preflight)`);
        }
    }
    if (u64(r, "candidates_checked", "result.") === "0") {
        throw new ResultError("result.candidates_checked is 0");
    }
    const argmax = BigInt(decString(r, "max_excursion_n", "result."));
    if (argmax < start || argmax >= end) {
        throw new ResultError("result.max_excursion_n is outside the unit");
    }
    const hits = records(r);
    let prev = -1n;
    for (const hit of hits) {
        const n = BigInt(hit.n);
        if (n < start || n >= end || n <= prev) {
            throw new ResultError("result.records are not ascending numbers inside the unit");
        }
        if (c.record_threshold === null || BigInt(hit.peak) <= BigInt(c.record_threshold)) {
            throw new ResultError("result.records holds a peak at or below the record threshold");
        }
        prev = n;
    }
    const claimed = text(r, "result_digest", "result.");
    if (!HEX64.test(claimed)) {
        throw new ResultError("result.result_digest is not 64 lowercase hex digits");
    }
    const digest = await sha256Hex(digestText(doc));
    if (digest !== claimed) {
        throw new ResultError("result.result_digest does not match the result fields");
    }
    return {
        digest,
        records: hits,
        canonical: canonicalDocument(doc),
        backend: bool(p, "gpu", "params.") ? "gpu" : "cpu",
        figures: figuresOf(doc),
    };
}
