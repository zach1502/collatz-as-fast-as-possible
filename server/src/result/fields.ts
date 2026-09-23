// Typed reads of verify --json fields; a missing or malformed field throws
// ResultError naming it.

import { JsonNumber, JsonObject, JsonValue, isObject, parseJson } from "../json";

/** A result that does not match its campaign or is malformed; the message is shown to the client. */
export class ResultError extends Error {}

export const DECIMAL = /^(0|[1-9][0-9]*)$/;
const U64_MAX = (1n << 64n) - 1n;

export function get(o: JsonObject, key: string, path: string): JsonValue {
    if (!Object.prototype.hasOwnProperty.call(o, key)) {
        throw new ResultError(`missing ${path}${key}`);
    }
    return o[key] as JsonValue;
}

export function obj(o: JsonObject, key: string, path: string): JsonObject {
    const v = get(o, key, path);
    if (!isObject(v)) {
        throw new ResultError(`${path}${key} is not an object`);
    }
    return v;
}

/** A JSON integer in [0, 2^64), returned as its canonical text. */
export function u64(o: JsonObject, key: string, path: string): string {
    const v = get(o, key, path);
    if (!(v instanceof JsonNumber) || !DECIMAL.test(v.raw) || BigInt(v.raw) > U64_MAX) {
        throw new ResultError(`${path}${key} is not an integer in [0, 2^64)`);
    }
    return v.raw;
}

export function u64OrNull(o: JsonObject, key: string, path: string): string | null {
    return get(o, key, path) === null ? null : u64(o, key, path);
}

export function decString(o: JsonObject, key: string, path: string): string {
    const v = get(o, key, path);
    if (typeof v !== "string" || !DECIMAL.test(v)) {
        throw new ResultError(`${path}${key} is not a canonical decimal string`);
    }
    return v;
}

export function bool(o: JsonObject, key: string, path: string): boolean {
    const v = get(o, key, path);
    if (typeof v !== "boolean") {
        throw new ResultError(`${path}${key} is not a boolean`);
    }
    return v;
}

export function text(o: JsonObject, key: string, path: string): string {
    const v = get(o, key, path);
    if (typeof v !== "string") {
        throw new ResultError(`${path}${key} is not a string`);
    }
    return v;
}

const PRINTABLE = /^[ -~]*$/;

/** A string of printable ASCII, 1 to `max` characters. */
export function plainText(o: JsonObject, key: string, path: string, max: number): string {
    const v = text(o, key, path);
    if (v.length === 0 || v.length > max || !PRINTABLE.test(v)) {
        throw new ResultError(`${path}${key} is not 1-${max} characters of printable ASCII`);
    }
    return v;
}

/** A JSON number written as a plain decimal (seconds_total is %.6f). */
export function plainNumber(o: JsonObject, key: string, path: string): string {
    const v = get(o, key, path);
    if (!(v instanceof JsonNumber) || !/^[0-9]{1,12}(\.[0-9]{1,9})?$/.test(v.raw)) {
        throw new ResultError(`${path}${key} is not a plain non-negative decimal number`);
    }
    return v.raw;
}

export interface RecordHit {
    n: string;
    peak: string;
}

export function records(res: JsonObject): RecordHit[] {
    const v = get(res, "records", "result.");
    if (!Array.isArray(v)) {
        throw new ResultError("result.records is not an array");
    }
    return v.map((e) => {
        if (
            !Array.isArray(e) ||
            e.length !== 2 ||
            typeof e[0] !== "string" ||
            typeof e[1] !== "string" ||
            !DECIMAL.test(e[0]) ||
            !DECIMAL.test(e[1])
        ) {
            throw new ResultError('result.records entry is not ["n", "peak"] with decimal strings');
        }
        return { n: e[0], peak: e[1] };
    });
}

/** The records of a document stored by canonicalDocument (unit_results, runs.result_json). */
export function storedRecords(text: string): RecordHit[] {
    const doc = parseJson(text);
    const r = isObject(doc) ? doc["result"] : undefined;
    return r !== undefined && isObject(r) ? records(r) : [];
}
