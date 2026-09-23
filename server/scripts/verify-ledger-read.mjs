// Reading a ledger for scripts/verify-ledger.mjs: a saved /api/ledger JSON
// array or /api/ledger.csv file, or a server's /api/ledger pages up to its head.

import { LINE_FIELDS_V1, LINE_FIELDS_V2 } from "./verify-ledger-check.mjs";

/** The fields of a published entry: the JSON keys and the CSV header. */
const COLUMNS = ["format", ...LINE_FIELDS_V2, "hash"];
/** The CSV header of a server without migration 0005, whose entries are all collatz-ledger/1. */
const COLUMNS_V1 = [...LINE_FIELDS_V1, "hash"];
const PAGE = 1000;

/** The rows of an RFC 4180 CSV text, as /api/ledger.csv writes it. */
function parseCsv(text) {
    const rows = [];
    let row = [];
    let field = "";
    let quoted = false;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (quoted) {
            if (c === '"' && text[i + 1] === '"') {
                field += '"';
                i++;
            } else if (c === '"') {
                quoted = false;
            } else {
                field += c;
            }
        } else if (c === '"') {
            quoted = true;
        } else if (c === ",") {
            row.push(field);
            field = "";
        } else if (c === "\r" || c === "\n") {
            if (c === "\r" && text[i + 1] === "\n") {
                i++;
            }
            row.push(field);
            rows.push(row);
            row = [];
            field = "";
        } else {
            field += c;
        }
    }
    if (field !== "" || row.length > 0) {
        row.push(field);
        rows.push(row);
    }
    return rows.filter((r) => r.length > 1 || r[0] !== "");
}

/**
 * The entries of a saved /api/ledger JSON array, or of /api/ledger.csv text
 * with the current header or that of a server without migration 0005; CSV
 * pages appended one after another repeat the header row, which is skipped,
 * and a byte order mark (a spreadsheet's) is ignored.
 */
export function parseSaved(text) {
    if (text.trimStart().startsWith("[")) {
        const v = JSON.parse(text);
        if (!Array.isArray(v)) {
            throw new Error("the JSON file is not an array of entries");
        }
        return v;
    }
    const [header, ...rows] = parseCsv(text.replace(/^\uFEFF/, ""));
    const columns = [COLUMNS, COLUMNS_V1].find((c) => header !== undefined && header.join(",") === c.join(","));
    if (columns === undefined) {
        throw new Error(`the CSV file does not start with the header ${COLUMNS.join(",")}`);
    }
    return rows
        .filter((r) => r.join(",") !== columns.join(","))
        .map((r, i) => {
            if (r.length !== columns.length) {
                throw new Error(`CSV row ${i + 1} has ${r.length} fields, not ${columns.length}`);
            }
            return Object.fromEntries(columns.map((k, j) => [k, r[j]]));
        });
}

/** GET `url` as JSON; a 429 or 503 is retried after the retry_after its body gives. */
async function getJson(url) {
    for (let attempt = 1; ; attempt++) {
        let r;
        try {
            r = await fetch(url, { headers: { accept: "application/json" } });
        } catch (e) {
            throw new Error(`cannot reach ${url}: ${e.cause?.message ?? e.message}`, { cause: e });
        }
        const text = await r.text();
        if ((r.status === 429 || r.status === 503) && attempt < 10) {
            const wait = Math.min(120, Number(/"retry_after":\s*([0-9]+)/.exec(text)?.[1] ?? 10));
            process.stderr.write(`${url}: HTTP ${r.status}, retrying in ${wait} s\n`);
            await new Promise((resolve) => setTimeout(resolve, wait * 1000));
            continue;
        }
        if (r.status !== 200) {
            throw new Error(`${url}: HTTP ${r.status} ${text.slice(0, 200)}`);
        }
        return JSON.parse(text);
    }
}

/**
 * The head {seq, hash, at} of the server at `base` and its entries up to the
 * head, page by page.
 */
export async function readServer(base) {
    const head = await getJson(`${base}/api/ledger/head`);
    if (!Number.isSafeInteger(head?.seq) || head.seq < 0 || typeof head.hash !== "string") {
        throw new Error(`${base}/api/ledger/head is not {seq, hash, at}`);
    }
    const entries = [];
    while (entries.length < head.seq) {
        const page = await getJson(`${base}/api/ledger?from=${entries.length + 1}&limit=${PAGE}`);
        if (!Array.isArray(page)) {
            throw new Error(`${base}/api/ledger is not a JSON array`);
        }
        if (page.length === 0) {
            // A page cached before the head moved (up to STATS_CACHE_SECONDS).
            throw new Error(`the pages of ${base}/api/ledger end at seq ${entries.length}, the head is seq ${head.seq}; try again in a minute`);
        }
        entries.push(...page);
    }
    return { head, entries: entries.slice(0, head.seq) };
}
