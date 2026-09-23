// For test/setup-jsonc.test.mjs: the cases on which it runs the functions that
// scripts/windows/setup-server.ps1 uses to edit server/wrangler.jsonc, built from the
// template server/wrangler.example.jsonc, which is only read, with the bytes and lines each
// case should write and print. The owner's wrangler.jsonc is never read.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const SERVER_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const TEMPLATE_PATH = path.join(SERVER_DIR, "wrangler.example.jsonc");
export const TEMPLATE = fs.readFileSync(TEMPLATE_PATH);
const BOM = Buffer.from([0xef, 0xbb, 0xbf]);

// Inputs: the template, normalized to LF, and live files made from it (a database id and an
// owner name set), in the line-end, indentation and BOM variants a Windows checkout
// can have.

const base = TEMPLATE.toString("utf8").replace(/^\uFEFF/, "").replaceAll("\r\n", "\n");
const ID_PLACEHOLDER = "REPLACE_WITH_YOUR_D1_ID";
const NAME_PLACEHOLDER = "REPLACE_WITH_YOUR_NAME";
assert.equal(base.split(`"database_id": "${ID_PLACEHOLDER}"`).length, 2, "the template has one database id placeholder");
assert.equal(base.split(`"OWNER_NAME": "${NAME_PLACEHOLDER}"`).length, 2, "the template has one owner name placeholder");
const ID = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
const OWNER = "Test Owner";
const NEW_ID = "11111111-2222-4333-8444-555555555555";
const NAME = 'Zach "Z" \\ 1502';
const NAME_JSON = 'Zach \\"Z\\" \\\\ 1502';
/** The value a block comment in `tricky` gives OWNER_NAME. */
const COMMENTED = "an earlier owner";
// A live file as the setup leaves it: the template with both values set.
const plain = base
    .replace(`"database_id": "${ID_PLACEHOLDER}"`, `"database_id": "${ID}"`)
    .replace(`"OWNER_NAME": "${NAME_PLACEHOLDER}"`, `"OWNER_NAME": "${OWNER}"`);
const crlf = (t) => t.replaceAll("\n", "\r\n");
const spaces = (t) => t.replace(/^\t+/gm, (m) => "  ".repeat(m.length));
// A block comment that names OWNER_NAME, a line comment that names the database id inside
// d1_databases, a string holding // and brackets, and a tab after a colon.
const tricky = plain
    .replace(/^([ \t]*)("OWNER_NAME": ")/m, `$1/* the old setting:\n$1"OWNER_NAME": "${COMMENTED}",\n$1*/\n$1$2`)
    .replace(/^([ \t]*)("database_id": ")/m, `$1// "database_id": "${ID}" names the live database\n$1$2`)
    .replace(/^([ \t]*)("MAX_BODY_BYTES")/m, '$1"DOC_URL": "https://example.com/a{b}[c]//d\\"e",\n$1$2')
    .replace(/^([ \t]*)"OWNER_NAME": "(?!an earlier)/m, '$1"OWNER_NAME":\t"');
// A live file from before the template: a second top-level D1 binding (remote) carries the
// same id, so -From changes two lines.
const twoIds = plain.replace(/^([ \t]*)\{\n[ \t]*"binding": "DB",\n[\s\S]*?\n\1\}$/m, (obj) =>
    `${obj},\n${obj.replace('"binding": "DB"', '"binding": "collatz_frontier"').replace('"migrations_dir": "migrations"', '"remote": true')}`);
assert.notEqual(twoIds, plain, "the second binding was added");
// A fresh copy of the template, also with spaces around the colon of OWNER_NAME.
const spacedColon = base.replace(`"OWNER_NAME": "`, `"OWNER_NAME" :  "`);

export const bytesOf = (text, bom = false) => Buffer.concat([bom ? BOM : Buffer.alloc(0), Buffer.from(text, "utf8")]);
const linesOf = (text, re) => text.split("\n").flatMap((l, i) => (re.test(l) ? [i + 1] : []));

// The oracle: `from` replaced by `to` (both as written between quotes) on line `no` only, by
// plain string replacement, independent of the patterns under test.
function withValue(text, no, from, to) {
    const lines = text.split("\n");
    assert.equal(lines[no - 1].split(`"${from}"`).length, 2, `line ${no} holds "${from}" once`);
    lines[no - 1] = lines[no - 1].replace(`"${from}"`, () => `"${to}"`);
    return lines.join("\n");
}

const set = (key, value, more = {}) => ({ op: "set", key, value, block: "vars", ...more });
const renameOps = [set("OWNER_NAME", NAME, { from: OWNER })];
const resetOps = [set("database_id", ID_PLACEHOLDER, { block: "d1_databases", from: ID })];
const firstRunOps = [
    set("database_id", NEW_ID, { block: "d1_databases", from: ID_PLACEHOLDER }),
    set("OWNER_NAME", NAME, { from: NAME_PLACEHOLDER }),
];

function rename(text) {
    const name = linesOf(text, new RegExp(`^[ \\t]*"OWNER_NAME":[ \\t]*"${OWNER}"`));
    return withValue(text, name[0], OWNER, NAME_JSON);
}
// Every top-level D1 binding that carries `from` gets `to`; only those lines carry it.
function withIds(text, from, to, count) {
    const ids = linesOf(text, new RegExp(`^[ \\t]*"database_id": "${from}"`));
    assert.equal(ids.length, count, `${count} top-level D1 binding(s) carry the id`);
    return ids.reduce((t, no) => withValue(t, no, from, to), text);
}
const reset = (text, count = 1) => withIds(text, ID, ID_PLACEHOLDER, count);
function firstRun(text) {
    const name = linesOf(text, /^[ \t]*"OWNER_NAME"/);
    return withValue(withIds(text, ID_PLACEHOLDER, NEW_ID, 1), name[0], NAME_PLACEHOLDER, NAME_JSON);
}

// Each case: input text and BOM, setup runs (each a list of operations, with a fresh backup
// flag), the text expected after them, and what each operation returns.
export const cases = [];
for (const [variant, text, bom] of [
    ["lf-tabs", plain, false], ["crlf-tabs", crlf(plain), false], ["lf-spaces", spaces(plain), false],
    ["crlf-bom", crlf(plain), true], ["tricky", tricky, false], ["tricky-crlf", crlf(tricky), false],
]) {
    cases.push({ name: `rename-${variant}`, text, bom, runs: [renameOps, renameOps], expected: rename(text), rets: [true, false] });
    cases.push({ name: `reset-${variant}`, text, bom, runs: [resetOps, resetOps], expected: reset(text), rets: [true, false] });
}
cases.push({ name: "reset-two-ids", text: twoIds, bom: false, runs: [resetOps, resetOps], expected: reset(twoIds, 2), rets: [true, false] });
for (const [variant, text, bom] of [["template", base, false], ["lf", spacedColon, false], ["crlf-bom", crlf(spacedColon), true]]) {
    cases.push({ name: `first-run-${variant}`, text, bom, runs: [firstRunOps, firstRunOps], expected: firstRun(text), rets: [true, true, false, false] });
}
// The same key twice in vars: without -From only the first line changes.
const LEASES = /"MAX_LEASES_PER_USER": "([^"]*)"/.exec(base)[1];
const dup = plain.replace(/^([ \t]*)("MAX_LEASES_PER_USER": "[^"]*",)$/m, "$1$2\n$1$2");
cases.push({
    name: "first-match-only", text: dup, bom: false, runs: [[set("MAX_LEASES_PER_USER", "99")]],
    expected: withValue(dup, linesOf(dup, /^[ \t]*"MAX_LEASES_PER_USER"/)[0], LEASES, "99"), rets: [true],
});
cases.push({
    name: "what-if", text: plain, bom: false, runs: [[set("OWNER_NAME", NAME, { from: OWNER, whatIf: true })]],
    expected: plain, rets: [false], wouldBe: rename(plain),
});
const MAX_BODY = /"MAX_BODY_BYTES": "([^"]*)"/.exec(base)[1];
const TEST_EMBARGO = /"env"[\s\S]*?"EMBARGO_SECONDS": "([^"]*)"/.exec(base)[1];
cases.push({
    name: "get", text: tricky, bom: false, expected: tricky, rets: [OWNER, ID, MAX_BODY, TEST_EMBARGO, null, null],
    runs: [[
        { op: "get", key: "OWNER_NAME", block: "vars" }, { op: "get", key: "database_id", block: "d1_databases" },
        { op: "get", key: "MAX_BODY_BYTES", block: "vars" }, { op: "get", key: "EMBARGO_SECONDS", block: "env" },
        { op: "get", key: "NO_SUCH_KEY", block: "vars" }, { op: "get", key: "OWNER_NAME", block: "no_such_block" },
    ]],
});
// Refused: the file stays as it is and no backup is written.
export const refusals = [
    { name: "missing-key", text: plain, op: set("NO_SUCH_KEY", "x"), message: /has no "NO_SUCH_KEY": "\.\.\." line in "vars"; add it by hand/ },
    { name: "missing-block", text: plain, op: set("OWNER_NAME", "x", { block: "no_such_block" }), message: /has no top-level "no_such_block"/ },
    { name: "bad-key", text: plain, op: set("OWNER-NAME", "x"), message: /bad key 'OWNER-NAME'/ },
    { name: "not-utf8", text: plain, raw: Buffer.concat([Buffer.from("// caf"), Buffer.from([0xe9]), Buffer.from("\n" + plain)]), op: set("OWNER_NAME", "x"), message: /is not UTF-8 text/ },
];

// The lines Set-JsoncValue prints for case `c`, from the case data: an operation that returns
// true prints "changed", and a -WhatIf one "would change" for the text in c.wouldBe. Each line
// names the lines where the text before and the text written differ and hold the key, and the
// value as written before and after.
export function printed(c) {
    const before = c.text.split("\n");
    return c.runs.flat().flatMap((op, i) => {
        if (c.rets[i] !== true && !op.whatIf) return [];
        const after = (op.whatIf ? c.wouldBe : c.expected).split("\n");
        const at = before.flatMap((l, n) => (l !== after[n] && l.includes(`"${op.key}"`) ? [n + 1] : []));
        const value = (l) => /"((?:[^"\\]|\\.)*)"/.exec(l.split(`"${op.key}"`)[1])[1];
        const verb = op.whatIf ? "would change" : "changed";
        return [`    ${verb} wrangler.jsonc line ${at.join(", ")}: "${op.key}": "${value(before[at[0] - 1])}" -> "${value(after[at[0] - 1])}"`];
    });
}
