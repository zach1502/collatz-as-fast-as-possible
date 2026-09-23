// npm test: path records, what /api/stats gives the page, and the page's
// assets. /api/stats gives only what the page shows. The Worker and
// the helpers are in test/helpers-owner.mjs.

import assert from "node:assert/strict";
import { test } from "node:test";

import { BARINA_L0, DEFAULT_ASSUMPTION } from "../scripts/campaign.mjs";
import {
    ADMIN_SECRET,
    CAMPAIGNS,
    LOW_THRESHOLD,
    N97,
    PEAK97,
    REC97,
    T97,
    acceptedAt,
    close,
    fetchAs,
    log2Of,
    newApi,
    pow,
    rec,
    recOf,
    resultFor,
    runOne,
    serveOwner,
    skip,
    srv,
    statsOf,
    submitAs,
    withHits,
} from "./helpers-owner.mjs";
import { newHost, parseExact } from "./helpers.mjs";

serveOwner();

test("path records: #97 alone, then a walk above the frontier is pending and becomes #98 once the frontier passes it", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.paths;
    let s = (await statsOf(id)).body;
    assert.deepEqual(s.path_records.map(rec), [REC97], "no hits: #97 alone");
    const [r97] = s.path_records;
    assert.equal((2n * T97).toString(), PEAK97, "the peak in the 3n+1 map is twice the T-map one, exactly");
    close(r97.peak_log2, log2Of(BigInt(PEAK97)), "log2 of #97's peak");

    // Blocks 0 and 1 on two machines; block 1 comes in first, with a walk just past #97's peak.
    const [h0, h1] = [newHost(), newHost()];
    const first = (await fetchAs(api, h0, id)).body.lease;
    const second = (await fetchAs(api, h1, id)).body.lease;
    assert.deepEqual([first.l, second.l], [BARINA_L0.toString(), (BARINA_L0 + pow(32)).toString()]);
    const peakT = T97 + 1n;
    const n = (BigInt(second.l) + 4321n).toString();
    assert.equal((await submitAs(api, h1, second.unit_id, resultFor(second, withHits([[4321n, peakT]])))).body.state, "accepted");
    s = (await statsOf(id)).body;
    assert.equal(s.frontier.value, BARINA_L0.toString(), "block 0 is not in yet");
    assert.deepEqual(s.path_records.map(rec), [REC97], "a walk above the frontier is pending, not listed");
    assert.equal(s.max_excursion.peak, (2n * peakT).toString(), "the largest peak counts every accepted block");

    assert.equal((await submitAs(api, h0, first.unit_id, resultFor(first))).body.state, "accepted");
    s = (await statsOf(id)).body;
    assert.equal(s.frontier.value, (BARINA_L0 + 2n * pow(32)).toString());
    const at = await acceptedAt(second.unit_id);
    assert.deepEqual(s.path_records.map(rec), [REC97, recOf(98, n, peakT, at)], "below the frontier it is #98, found when its block was accepted");
    assert.equal(BigInt(s.path_records[1].peak) - BigInt(PEAK97), 2n, "exact: one more in the T-map is two more in the 3n+1 map");
    close(s.path_records[1].peak_log2, log2Of(2n * peakT), "log2 of #98's peak");
});

test("path records: a walk is one only if its peak beats #97's and that of every smaller walk", { skip }, async () => {
    const api = newApi();
    // Its threshold is below #97's peak, so the blocks also list walks that do not beat #97.
    const id = CAMPAIGNS.pathsMax;
    const host = newHost();
    const e = 10n ** 30n;
    const low = (hits) => (d) => {
        d.params.record_threshold = LOW_THRESHOLD;
        withHits(hits)(d);
    };
    const b0 = await runOne(api, host, id, {}, low([[10n, T97], [20n, T97 + e], [30n, T97 + e / 10n], [40n, T97 + e], [50n, T97 + 2n * e]]));
    const b1 = await runOne(api, host, id, {}, low([[5n, T97 + 2n * e - 1n], [6n, T97 + 5n * e]]));
    const s = (await statsOf(id)).body;
    assert.equal(s.frontier.value, (BARINA_L0 + 2n * pow(32)).toString());
    const [start0, start1] = [BigInt(b0.lease.l), BigInt(b1.lease.l)];
    const [at0, at1] = [await acceptedAt(b0.lease.unit_id), await acceptedAt(b1.lease.unit_id)];
    assert.deepEqual(
        s.path_records.map(rec),
        [
            REC97,
            recOf(98, (start0 + 20n).toString(), T97 + e, at0),
            recOf(99, (start0 + 50n).toString(), T97 + 2n * e, at0),
            recOf(100, (start1 + 6n).toString(), T97 + 5n * e, at1),
        ],
        "not #97's own peak, a lower or an equal peak after #98, or a lower peak in the next block",
    );
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM records WHERE campaign_id = ${id}`))[0].n, 7, "every walk is kept");
});

test("path records: a block that leaves the frontier takes its records along, and brings them back when accepted again", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.pathsBack; // recheck_rate 1, RECHECK_EVERY 0: the owner's next fetch is the re-check
    const host = newHost();
    const peakT = T97 + 777n;
    const edit = withHits([[99n, peakT]]);
    const u = await runOne(api, host, id, {}, edit);
    const unitId = u.lease.unit_id;
    const n = (BigInt(u.lease.l) + 99n).toString();
    let s = (await statsOf(id)).body;
    assert.deepEqual(s.path_records.map(rec), [REC97, recOf(98, n, peakT, await acceptedAt(unitId))]);

    // The re-check disagrees: the unit is in conflict, its walks are dropped and the frontier rolls back.
    const f = await fetchAs(api, host, id);
    assert.equal(f.body.lease.unit_id, unitId);
    const wrong = await submitAs(
        api,
        host,
        unitId,
        resultFor(f.body.lease, (d) => {
            edit(d);
            d.result.blocks_total = Number(String(d.result.blocks_total).replaceAll("@", "")) + 1;
        }),
    );
    assert.equal(wrong.status, 200, wrong.text);
    assert.equal((await srv.query(`SELECT state FROM units WHERE id = ${unitId}`))[0].state, "conflict");
    s = (await statsOf(id)).body;
    assert.deepEqual([s.frontier.value, s.path_records.map(rec)], [BARINA_L0.toString(), [REC97]]);
    // A walk left behind by a unit that is not accepted would not count
    // either, not even as pending (it lies above the frontier).
    await srv.sql(`INSERT INTO records (campaign_id, n, peak, unit_id, users, found_at) VALUES (${id}, '${n}', '${peakT}', ${unitId}, 'owner', 1);`);
    s = (await statsOf(id)).body;
    assert.deepEqual(s.path_records.map(rec), [REC97]);
    await srv.sql(`DELETE FROM records WHERE campaign_id = ${id};`);

    // The owner accepts the first result again: #98 is back, found at the unit's second acceptance.
    const digest = parseExact(resultFor(u.lease, edit)).result.result_digest;
    const dec = await api.post("/api/admin/decide", { unit_id: unitId, digest, note: "the first result stands" }, { authorization: `Bearer ${ADMIN_SECRET}` });
    assert.equal(dec.status, 200, dec.text);
    s = (await statsOf(id)).body;
    assert.deepEqual(s.path_records.map(rec), [REC97, recOf(98, n, peakT, await acceptedAt(unitId))]);
});

test("path records: none for a campaign whose blocks need not list every walk above #97's peak", { skip }, async () => {
    for (const id of [CAMPAIGNS.pathsOff, CAMPAIGNS.pathsHigh]) {
        const s = (await statsOf(id)).body;
        assert.equal(s.path_records, null, `campaign ${id}`);
    }
});

test("/api/stats gives what the page shows and plain counts, and no word of the verifier", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.page;
    // The campaign holds what /api/stats leaves out: the assumption source
    // citing Barina, the record threshold, the kernel, the digest schema.
    const [row] = await srv.query(`SELECT assumption_source, record_threshold, kernel, digest_schema FROM campaigns WHERE id = ${id}`);
    assert.match(row.assumption_source, /Barina/);
    assert.deepEqual([row.record_threshold !== null, row.kernel, row.digest_schema], [true, "table16", "collatz-result-digest/2"]);
    const big = "9".repeat(45);
    await runOne(api, newHost(), id, {}, (d) => {
        d.params.assumption_source = DEFAULT_ASSUMPTION;
        d.result.seconds_total = 5;
        d.result.max_excursion = big;
        d.result.max_excursion_n = (BigInt(d.block.L) + 7n).toString();
        d.result.records = [[d.result.max_excursion_n, big]];
    });
    assert.equal((await srv.query(`SELECT COUNT(*) AS n FROM records WHERE campaign_id = ${id}`))[0].n, 1, "the block has a record");

    // Exactly the fields public/app.js reads, and plain counts and times.
    const s = (await statsOf(id)).body;
    const keys = (o) => Object.keys(o).sort();
    assert.deepEqual(keys(s), [
        "alerts_open",
        "audits",
        "blocks",
        "campaign",
        "frontier",
        "generated_at",
        "history",
        "hosts",
        "ledger",
        "max_excursion",
        "milestones",
        "owner_name",
        "path_records",
        "speed",
        "start_credit",
        "totals",
    ]);
    assert.deepEqual(s.start_credit, { name: "David Barina", url: "https://pcbarina.fit.vutbr.cz/" });
    assert.deepEqual(keys(s.campaign), ["id", "name", "start", "start_log2"]);
    assert.deepEqual(keys(s.ledger), ["entries", "head", "head_full"]);
    assert.deepEqual(s.audits, { passed: 0, failed: 0 });
    assert.deepEqual(keys(s.frontier), ["log2", "value"]);
    assert.deepEqual(keys(s.totals), ["blocks", "last_block_at", "numbers_verified"]);
    assert.deepEqual(keys(s.speed), ["all_time", "blocks_per_day", "last_10_min", "last_24_h", "last_hour"]);
    for (const w of ["last_10_min", "last_hour", "last_24_h", "all_time"]) {
        assert.deepEqual(keys(s.speed[w]), ["blocks", "numbers_per_second", "seconds"], w);
    }
    assert.equal(s.blocks.length, 1);
    assert.deepEqual(keys(s.blocks[0]), ["k", "numbers_per_second", "seconds", "submitted_at"]);
    assert.ok(s.milestones.length > 0);
    for (const m of s.milestones) {
        assert.deepEqual(keys(m), ["eta_at", "eta_seconds", "label"]);
    }
    assert.deepEqual(keys(s.history), ["daily", "hourly"]);
    for (const b of [...s.history.hourly, ...s.history.daily]) {
        assert.deepEqual(keys(b), ["blocks", "numbers", "start"]);
    }
    assert.equal(s.hosts.length, 1);
    assert.deepEqual(keys(s.hosts[0]), ["backend", "blocks", "host", "last_at", "numbers", "numbers_per_second"]);
    assert.deepEqual(s.max_excursion, { peak: (2n * BigInt(big)).toString(), peak_log2: s.max_excursion.peak_log2, n: s.max_excursion.n });
    // #97 and the block's walk, which beats it below the frontier.
    assert.deepEqual(s.path_records.map((r) => r.number), [97, 98]);
    for (const r of s.path_records) {
        assert.deepEqual(keys(r), ["found_at", "known", "n", "number", "peak", "peak_log2"]);
    }

    // No /api/stats answer, for any campaign or the default one, holds a
    // verifier term. The campaign's name is the owner's choice ("recheck"
    // here) and the start credit names Barina, so both are blanked; his name
    // anywhere else would mean that the campaign's assumption source leaked.
    const banned = /barina|digest|kernel|commit|assumption|quorum|recheck|candidates|overflow|unwalked/i;
    for (const q of ["", ...Object.values(CAMPAIGNS).map((c) => `?campaign=${c}`)]) {
        const r = await newApi().get(`/api/stats${q}`);
        assert.equal(r.status, 200, `${q}: ${r.text}`);
        assert.equal(r.body.start_credit.name, "David Barina", q);
        const rest = { ...r.body, start_credit: null, campaign: { ...r.body.campaign, name: "" } };
        assert.doesNotMatch(JSON.stringify(rest), banned, `/api/stats${q}`);
    }
});

test("the page and its assets are served with the CSP and load nothing from other origins", { skip }, async () => {
    const api = newApi();
    for (const p of ["/", "/app.js", "/style.css", "/path-records.json"]) {
        const r = await api.get(p);
        assert.equal(r.status, 200, p);
        const csp = r.headers.get("content-security-policy") ?? "";
        assert.match(csp, /default-src 'none'/, p);
        assert.match(csp, /script-src 'self'/, p);
        assert.match(csp, /style-src 'self'/, p);
        assert.match(csp, /connect-src 'self'/, p);
        assert.doesNotMatch(csp, /unsafe|\*/, p);
        assert.equal(r.headers.get("x-content-type-options"), "nosniff", p);
    }
    const published = await api.get("/path-records.json");
    assert.match(published.headers.get("content-type") ?? "", /^application\/json/);
    assert.deepEqual(published.body.map((r) => r.number), Array.from({ length: 97 }, (_, i) => i + 1), "records #1 to #97");
    assert.deepEqual(published.body[96], { number: 97, n: N97, peak: PEAK97, discoverer: "David Barina" }, "the #97 that /api/stats gives");
    const page = (await api.get("/")).text;
    assert.doesNotMatch(page, /<script[^>]*src="(https?:)?\/\//i, "no script from another origin");
    assert.doesNotMatch(page, /<link[^>]*href="(https?:)?\/\//i, "no stylesheet or font from another origin");
    assert.doesNotMatch(page, /style="/, "no inline styles (style-src 'self')");
    assert.doesNotMatch(page, /<script>/, "no inline script");
    const ids = ["frontier", "start", "start-value", "start-credit", "start-credit-link", "checked", "audits", "speed", "eta", "hourly-chart", "daily-chart", "daily-table", "hosts", "blocks", "peak", "path-records", "path-records-table", "path-records-all", "path-records-all-n", "path-records-all-table"];
    for (const idName of ids) {
        assert.match(page, new RegExp(`id="${idName}"`), idName);
    }
    // The start line is hidden until the numbers load, its credit until /api/stats names someone.
    assert.match(page, /<p id="start" hidden>Every number below <span id="start-value"><\/span> had already been checked<span id="start-credit" hidden> by <a id="start-credit-link"><\/a>'s project<\/span>\. This verifier continues from there\.<\/p>/);
    const js = (await api.get("/app.js")).text;
    for (const name of ["start", "start-value", "start-credit", "start-credit-link"]) {
        assert.ok(js.includes(`$("${name}")`), name);
    }
    assert.match(js, /\$\("start-credit"\)\.hidden = !credit;/, "no credit, no credit text");
    // The audit line is hidden until the answer has audits.
    assert.match(page, /<p id="audits" class="quiet" hidden><\/p>/);
    assert.match(js, /\$\("audits"\)\.hidden = !a;/);
    assert.ok(js.includes("`Audits: ${a.passed} passed, ${a.failed} failed.`"), "the audit line");
    // Barina is named only through /api/stats (start_credit): the files name nobody.
    for (const [name, text] of [["page", page], ["script", js]]) {
        assert.doesNotMatch(text, /barina|readme|github|digest|quorum|re-?check|lease|candidate|overflow|sieve|kernel|commit/i, name);
    }
    assert.doesNotMatch(js, /fetch\(\s*["'`]https?:/, "the script fetches only this origin");
    assert.doesNotMatch(js, /\.(innerHTML|outerHTML)\b|insertAdjacentHTML|document\.write/, "data never goes through HTML parsing");
    assert.doesNotMatch(js, /setAttribute\(\s*["']style/, "no inline styles");
});
