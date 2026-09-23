// JSON parser that keeps every number as its source text. Result documents
// carry u64 counters above 2^53, which JSON.parse would round. Duplicate keys,
// control characters in strings and trailing data are rejected.

export class JsonNumber {
    constructor(readonly raw: string) {}
}

export type JsonValue = null | boolean | string | JsonNumber | JsonValue[] | JsonObject;
export interface JsonObject {
    [key: string]: JsonValue;
}

export class JsonSyntaxError extends Error {}

const NUMBER = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;

export function parseJson(text: string, maxDepth = 32): JsonValue {
    let i = 0;
    const n = text.length;
    const fail = (what: string): never => {
        throw new JsonSyntaxError(`${what} at offset ${i}`);
    };
    const ws = (): void => {
        while (i < n) {
            const c = text.charCodeAt(i);
            if (c !== 0x20 && c !== 0x0a && c !== 0x0d && c !== 0x09) {
                break;
            }
            i++;
        }
    };
    const str = (): string => {
        i++; // opening quote
        let out = "";
        let start = i;
        for (;;) {
            if (i >= n) {
                fail("unterminated string");
            }
            const c = text.charCodeAt(i);
            if (c === 0x22) {
                out += text.slice(start, i);
                i++;
                return out;
            }
            if (c < 0x20) {
                fail("control character in string");
            }
            if (c !== 0x5c) {
                i++;
                continue;
            }
            out += text.slice(start, i);
            i++;
            const e = text[i++];
            switch (e) {
                case '"':
                case "\\":
                case "/":
                    out += e;
                    break;
                case "b":
                    out += "\b";
                    break;
                case "f":
                    out += "\f";
                    break;
                case "n":
                    out += "\n";
                    break;
                case "r":
                    out += "\r";
                    break;
                case "t":
                    out += "\t";
                    break;
                case "u": {
                    const hex = text.slice(i, i + 4);
                    if (!/^[0-9a-fA-F]{4}$/.test(hex)) {
                        fail("bad \\u escape");
                    }
                    out += String.fromCharCode(parseInt(hex, 16));
                    i += 4;
                    break;
                }
                default:
                    fail("bad escape");
            }
            start = i;
        }
    };
    const value = (depth: number): JsonValue => {
        if (depth > maxDepth) {
            fail("nesting too deep");
        }
        ws();
        if (i >= n) {
            fail("unexpected end of input");
        }
        const c = text[i];
        if (c === "{") {
            i++;
            const obj: JsonObject = Object.create(null) as JsonObject;
            ws();
            if (text[i] === "}") {
                i++;
                return obj;
            }
            for (;;) {
                ws();
                if (text[i] !== '"') {
                    fail("expected string key");
                }
                const key = str();
                if (Object.prototype.hasOwnProperty.call(obj, key)) {
                    fail(`duplicate key "${key}"`);
                }
                ws();
                if (text[i] !== ":") {
                    fail("expected ':'");
                }
                i++;
                obj[key] = value(depth + 1);
                ws();
                if (text[i] === ",") {
                    i++;
                    continue;
                }
                if (text[i] === "}") {
                    i++;
                    return obj;
                }
                fail("expected ',' or '}'");
            }
        }
        if (c === "[") {
            i++;
            const arr: JsonValue[] = [];
            ws();
            if (text[i] === "]") {
                i++;
                return arr;
            }
            for (;;) {
                arr.push(value(depth + 1));
                ws();
                if (text[i] === ",") {
                    i++;
                    continue;
                }
                if (text[i] === "]") {
                    i++;
                    return arr;
                }
                fail("expected ',' or ']'");
            }
        }
        if (c === '"') {
            return str();
        }
        if (text.startsWith("true", i)) {
            i += 4;
            return true;
        }
        if (text.startsWith("false", i)) {
            i += 5;
            return false;
        }
        if (text.startsWith("null", i)) {
            i += 4;
            return null;
        }
        NUMBER.lastIndex = i;
        const m = NUMBER.exec(text);
        if (m === null) {
            return fail("unexpected character");
        }
        i = NUMBER.lastIndex;
        return new JsonNumber(m[0]);
    };
    const v = value(0);
    ws();
    if (i !== n) {
        fail("trailing data");
    }
    return v;
}

/** Serializes a parsed value back to JSON; numbers keep their source text. */
export function stringifyJson(v: JsonValue): string {
    if (v === null) {
        return "null";
    }
    if (typeof v === "boolean") {
        return v ? "true" : "false";
    }
    if (typeof v === "string") {
        return JSON.stringify(v);
    }
    if (v instanceof JsonNumber) {
        return v.raw;
    }
    if (Array.isArray(v)) {
        return "[" + v.map(stringifyJson).join(",") + "]";
    }
    return (
        "{" +
        Object.keys(v)
            .map((k) => JSON.stringify(k) + ":" + stringifyJson(v[k] as JsonValue))
            .join(",") +
        "}"
    );
}

export function isObject(v: JsonValue | undefined): v is JsonObject {
    return typeof v === "object" && v !== null && !(v instanceof JsonNumber) && !Array.isArray(v);
}
