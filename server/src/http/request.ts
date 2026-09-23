// Errors, JSON responses, the D1 wrapper that counts rows, the request body
// reader and the checks of request fields.

import { JsonObject, JsonSyntaxError, isObject, parseJson } from "../json";
import { Env, intVar } from "./env";

export class HttpError extends Error {
    constructor(
        readonly status: number,
        message: string,
        readonly extra: Record<string, unknown> = {},
    ) {
        super(message);
    }
}

export function json(status: number, body: unknown, headers: Record<string, string> = {}): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: {
            "content-type": "application/json; charset=utf-8",
            "cache-control": "no-store",
            "x-content-type-options": "nosniff",
            ...headers,
        },
    });
}

/** D1 access that sums rows read/written over a request (reported in test mode). */
export class Db {
    rowsRead = 0;
    rowsWritten = 0;

    constructor(readonly d1: D1Database) {}

    prep(sql: string, ...args: unknown[]): D1PreparedStatement {
        return this.d1.prepare(sql).bind(...args);
    }

    private track(meta: Partial<Pick<D1Meta, "rows_read" | "rows_written">>): void {
        this.rowsRead += meta.rows_read ?? 0;
        this.rowsWritten += meta.rows_written ?? 0;
    }

    async all<T>(sql: string, ...args: unknown[]): Promise<T[]> {
        const r = await this.prep(sql, ...args).all<T>();
        this.track(r.meta);
        return r.results;
    }

    async first<T>(sql: string, ...args: unknown[]): Promise<T | null> {
        return (await this.all<T>(sql, ...args))[0] ?? null;
    }

    async run(sql: string, ...args: unknown[]): Promise<D1Meta> {
        const r = await this.prep(sql, ...args).run();
        this.track(r.meta);
        return r.meta;
    }

    async batch(stmts: D1PreparedStatement[]): Promise<D1Result[]> {
        const rs = await this.d1.batch(stmts);
        for (const r of rs) {
            this.track(r.meta);
        }
        return rs;
    }
}

export const nowSeconds = (): number => Math.floor(Date.now() / 1000);

/**
 * The body cap of `endpoint`: MAX_SUBMIT_BYTES (default 256 KB) for
 * /api/submit, whose result lists every record of the block (about 80 bytes
 * each, so 32 KB held only about 400), and MAX_BODY_BYTES (default 32 KB) for
 * every other request.
 */
export function bodyCap(env: Env, endpoint: string): number {
    return endpoint === "submit" ? intVar(env.MAX_SUBMIT_BYTES, 262144) : intVar(env.MAX_BODY_BYTES, 32768);
}

/** Reads at most `cap` bytes (bodyCap) of UTF-8 and parses them as one JSON object. */
export async function readBody(request: Request, cap: number): Promise<JsonObject> {
    const declared = Number(request.headers.get("content-length") ?? "0");
    if (declared > cap) {
        throw new HttpError(413, `request body is over ${cap} bytes`);
    }
    const chunks: Uint8Array[] = [];
    let total = 0;
    if (request.body !== null) {
        const reader = request.body.getReader() as ReadableStreamDefaultReader<Uint8Array>;
        for (;;) {
            const { done, value } = await reader.read();
            if (done) {
                break;
            }
            total += value.byteLength;
            if (total > cap) {
                await reader.cancel();
                throw new HttpError(413, `request body is over ${cap} bytes`);
            }
            chunks.push(value);
        }
    }
    const bytes = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        bytes.set(c, off);
        off += c.byteLength;
    }
    let body;
    try {
        body = parseJson(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes));
    } catch (e) {
        throw new HttpError(400, e instanceof JsonSyntaxError ? `invalid JSON: ${e.message}` : "body is not UTF-8 JSON");
    }
    if (!isObject(body)) {
        throw new HttpError(400, "body must be a JSON object");
    }
    return body;
}

const HOST = /^[0-9a-f]{16,64}$/;

export function checkHost(v: unknown): string {
    if (typeof v !== "string" || !HOST.test(v)) {
        throw new HttpError(400, "host must be 16-64 lowercase hex digits");
    }
    return v;
}

/** Compares two strings in time that depends only on their lengths. */
export function sameString(a: string, b: string): boolean {
    if (a.length !== b.length) {
        return false;
    }
    let d = 0;
    for (let i = 0; i < a.length; i++) {
        d |= a.charCodeAt(i) ^ b.charCodeAt(i);
    }
    return d === 0;
}

/**
 * POST bodies must be declared as JSON (415 otherwise). Every client sends
 * it, and a browser cannot send it cross-site without a CORS preflight, which
 * this Worker does not answer, so no page can post forms to the API.
 */
export function requireJsonBody(request: Request): void {
    const type = (request.headers.get("content-type") ?? "").split(";")[0]?.trim().toLowerCase();
    if (type !== "application/json") {
        throw new HttpError(415, "the body must be sent with Content-Type: application/json");
    }
}
