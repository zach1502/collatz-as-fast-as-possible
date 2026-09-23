// The digest text of a verify --json document (checkpoint::result_digest_text
// in src/core/checkpoint_digest.cpp), its SHA-256, and the digest fields in
// which stored documents differ.

import { JsonObject, isObject, parseJson } from "../json";
import { DECIMAL, ResultError, bool, decString, get, obj, records, text, u64, u64OrNull } from "./fields";

/**
 * First line of the digest text (checkpoint::kResultDigestSchema), and the
 * only value of campaigns.digest_schema and result.result_digest_schema this
 * server accepts. /2: overflow_fallbacks counts only walks that passed 2^128-1.
 */
export const DIGEST_SCHEMA = "collatz-result-digest/2";

/** The canonical digest text of a verify --json document (checkpoint::result_digest_text). */
export function digestText(doc: JsonObject): string {
    const p = obj(doc, "params", "");
    const r = obj(doc, "result", "");
    const pre = obj(doc, "preflight", "");
    const wc = u64OrNull(p, "window_chains", "params.");
    const wd = u64OrNull(p, "window_templates_D", "params.");
    if ((wc === null) !== (wd === null)) {
        throw new ResultError("params.window_chains and params.window_templates_D must both be known or both null");
    }
    const depth = u64OrNull(p, "unwalked_depth", "params.");
    const threshold = get(p, "record_threshold", "params.");
    if (threshold !== null && (typeof threshold !== "string" || !DECIMAL.test(threshold))) {
        throw new ResultError("params.record_threshold is neither null nor a decimal string");
    }
    const maxExact = get(r, "max_exact", "result.");
    if (maxExact !== null && typeof maxExact !== "boolean") {
        throw new ResultError("result.max_exact is not a boolean or null");
    }
    const exactLine = maxExact === true ? "1" : maxExact === false && depth !== null ? "0" : "?";
    const lines: Array<[string, string]> = [
        ["block_h", decString(p, "block_h", "params.")],
        ["K", u64(p, "N", "params.")],
        ["assumes_below", decString(p, "assumes_below", "params.")],
        ["case", `${u64(p, "case_c", "params.")}/${u64(p, "case_k", "params.")}`],
        ["A", u64(p, "A", "params.")],
        ["B", u64(p, "B", "params.")],
        ["bitvectors", u64(p, "bitvectors", "params.")],
        ["kernel", text(p, "kernel", "params.")],
        ["mod3t", u64(p, "mod3t", "params.")],
        ["partial_oee", bool(p, "partial_oee", "params.") ? "1" : "0"],
        ["window_chains", wc ?? "?"],
        ["window_templates_D", wd ?? "?"],
        ["unwalked_depth", depth ?? "?"],
        ["record_threshold", threshold === null ? "-" : threshold],
        ["candidates_checked", u64(r, "candidates_checked", "result.")],
        ["step2_masked_out", u64(r, "step2_masked_out", "result.")],
        ["step1_survivors", u64(r, "step1_survivors", "result.")],
        ["step3_steps_total", u64(r, "step3_steps_total", "result.")],
        ["blocks_total", u64(r, "blocks_total", "result.")],
        ["coverage_checksum", u64(r, "coverage_checksum", "result.")],
        ["pow_checksum", u64(r, "pow_checksum", "result.")],
        ["pow_kernel_id", u64(r, "pow_kernel_id", "result.")],
        ["max_excursion", decString(r, "max_excursion", "result.")],
        ["max_excursion_n", decString(r, "max_excursion_n", "result.")],
        ["max_exceeds_u128", bool(r, "max_exceeds_u128", "result.") ? "1" : "0"],
        ["max_exact", exactLine],
        ["overflow_fallbacks", u64(r, "overflow_fallbacks", "result.")],
        [
            "preflight",
            [
                u64(pre, "candidates", "preflight."),
                u64(pre, "coverage_checksum", "preflight."),
                u64(pre, "pow_checksum", "preflight."),
                u64(pre, "blocks_total", "preflight."),
                u64(pre, "step3_steps_total", "preflight."),
            ].join(","),
        ],
        ["records", records(r).map((h) => `${h.n}:${h.peak}`).join(";")],
        ["records_truncated", bool(r, "records_truncated", "result.") ? "1" : "0"],
    ];
    return DIGEST_SCHEMA + "\n" + lines.map(([k, v]) => `${k}=${v}\n`).join("");
}

export async function sha256Hex(s: string): Promise<string> {
    const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
    return Array.from(new Uint8Array(d), (b) => b.toString(16).padStart(2, "0")).join("");
}

/** The digest-text lines (field -> value) of a stored document; empty for text that is not one. */
function digestFields(text: string): Map<string, string> {
    try {
        const doc = parseJson(text);
        if (!isObject(doc)) {
            return new Map();
        }
        const lines = digestText(doc).split("\n").slice(1).filter((l) => l !== "");
        return new Map(lines.map((l) => [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)]));
    } catch {
        return new Map();
    }
}

/**
 * The digest-text fields (blocks_total, pow_checksum, records, ...) whose
 * values differ among the stored documents `texts`, in digest-text order:
 * which parts of two disagreeing results differ, without their values.
 */
export function fieldDiff(texts: string[]): string[] {
    const maps = texts.map(digestFields).filter((m) => m.size > 0);
    const first = maps[0];
    if (first === undefined) {
        return [];
    }
    return [...first.keys()].filter((k) => maps.some((m) => m.get(k) !== first.get(k)));
}
