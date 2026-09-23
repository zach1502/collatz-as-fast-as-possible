// npm test: public/path-records.json, the published path records the page
// lists before those this server finds, against data/path-records.csv and
// the #97 the server numbers from (src/stats/path_records.ts RECORD97). No
// Worker runs.

import assert from "node:assert/strict";
import fs from "node:fs";
import { test } from "node:test";

import { CSV_PATH, JSON_PATH, buildPathRecords } from "../scripts/build-path-records.mjs";
import { RECORD97_PEAK_T } from "../scripts/campaign.mjs";

const csv = fs.readFileSync(CSV_PATH, "utf8");
const text = fs.readFileSync(JSON_PATH, "utf8");

test("public/path-records.json is what scripts/build-path-records.mjs builds from data/path-records.csv", () => {
    assert.equal(text.replaceAll("\r\n", "\n"), buildPathRecords(csv), "run node scripts/build-path-records.mjs");
});

test("public/path-records.json: records #1 to #97 with exact integers, n and peak increasing", () => {
    const rows = JSON.parse(text);
    assert.equal(rows.length, 97);
    rows.forEach((r, i) => {
        assert.deepEqual(Object.keys(r), ["number", "n", "peak", "discoverer"], `#${i + 1}`);
        assert.equal(r.number, i + 1);
        assert.match(r.n, /^[1-9][0-9]*$/);
        assert.match(r.peak, /^[1-9][0-9]*$/);
        assert.ok(r.discoverer === null || (typeof r.discoverer === "string" && r.discoverer !== ""), `#${r.number}`);
        if (i > 0) {
            assert.ok(BigInt(r.n) > BigInt(rows[i - 1].n) && BigInt(r.peak) > BigInt(rows[i - 1].peak), `#${r.number}`);
        }
    });
    assert.deepEqual(rows[0], { number: 1, n: "2", peak: "2", discoverer: null });
    assert.deepEqual(rows[4], { number: 5, n: "27", peak: "9232", discoverer: null });
    assert.deepEqual(rows[96], {
        number: 97,
        n: "2358909599867980429759",
        peak: "988229361387264324637139131613488937796184",
        discoverer: "David Barina",
    });
    assert.equal(rows[96].peak, (2n * BigInt(RECORD97_PEAK_T)).toString(), "twice the T-map peak the server's #98 has to beat");
    assert.deepEqual(
        [...new Set(rows.map((r) => r.discoverer))],
        [null, "Leavens & Vermeulen", "Tomas Oliveira e Silva", "Eric Roosendaal", "Tomas Oliveira e Silva (2004)", "Tomas Oliveira e Silva (2005)", "yoyo@home project (2017)", "David Barina"],
    );
});

test("scripts/build-path-records.mjs refuses a wrong peak, a row out of number, a peak that sets no record and a bad header or source", () => {
    const lines = csv.split("\n");
    const edit = (i, line) => lines.map((l, k) => (k === i ? line : l)).join("\n");
    assert.throws(() => buildPathRecords(edit(5, lines[5].replace(",9232,", ",9233,"))), /line 6: the orbit of 27 peaks at 9232, not 9233/);
    assert.throws(() => buildPathRecords(edit(5, lines[5].replace(/^5,/, "6,"))), /line 6: number 6, expected 5/);
    // 26 peaks at 40, below #4's 160.
    assert.throws(() => buildPathRecords(edit(5, lines[5].replace(",27,9232,", ",26,40,"))), /line 6: n and peak must both exceed those of the record before/);
    assert.throws(() => buildPathRecords(edit(0, "number,n,peak,discoverer")), /line 1 must be number,n,peak,discoverer,source/);
    assert.throws(() => buildPathRecords(edit(10, lines[10].replace(",barina;", ",barina; "))), /line 11: discoverer must be printable ASCII/);
});
