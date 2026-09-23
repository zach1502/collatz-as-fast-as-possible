// npm test: GET /api/stats: per-block figures, totals and
// machines, a disagreeing re-check, and the stats cache. The stats math is
// checked on block_stats rows written with SQL at known times, read with
// X-Test-Now; the stats cache is off unless a request turns it on with
// X-Test-Stats-Cache-Seconds. The Worker and the helpers are in
// test/helpers-owner.mjs.

import assert from "node:assert/strict";
import { test } from "node:test";

import { BARINA_L0 } from "../scripts/campaign.mjs";
import {
    ADMIN_SECRET,
    CAMPAIGNS,
    N97,
    OWNER_NAME,
    close,
    fetchAs,
    measure,
    newApi,
    nowS,
    pow,
    resultFor,
    runOne,
    serveOwner,
    skip,
    srv,
    statsOf,
    submitAs,
    sum,
} from "./helpers-owner.mjs";
import { newHost, parseExact } from "./helpers.mjs";

serveOwner();

// The number a milestone label names, as a float: "2076*2^60" or "2^71.1".
const milestone = (label) => label.split("*").reduce((x, f) => x * (f.startsWith("2^") ? 2 ** Number(f.slice(2)) : Number(f)), 1);

test("stats through the API: per-block figures, totals, machines and the largest excursion", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.stats;
    const [h1, h2] = [newHost(), newHost()];
    const big = "9".repeat(45);
    let recordN = null;
    const b1 = await runOne(api, h1, id, { bits: 33 }, (d) => (d.result.seconds_total = 4));
    const b2 = await runOne(api, h2, id, { bits: 32 }, (d) => {
        d.result.seconds_total = 1;
        recordN = (BigInt(d.block.L) + 12345n).toString();
        d.result.max_excursion = big;
        d.result.max_excursion_n = recordN;
        d.result.records = [[recordN, big]];
    });
    const b3 = await runOne(api, h1, id, { bits: 34 }, (d) => (d.result.seconds_total = 2));
    measure("submit (accepts a later block)", b3.submitRes);
    assert.deepEqual([b1.lease.k, b2.lease.k, b3.lease.k], [33, 32, 34]);
    const r = await statsOf(id);
    measure("stats (three blocks)", r);
    const s = r.body;
    assert.equal(r.headers.get("x-stats-cache"), "miss");
    assert.equal(r.headers.get("cache-control"), "no-store", "the test environment does not cache");
    assert.equal(s.owner_name, OWNER_NAME);
    assert.deepEqual(s.campaign, { id, name: "stats", start: s.campaign.start, start_log2: s.campaign.start_log2 }, "no parameters of the campaign");
    assert.deepEqual(
        s.totals,
        { numbers_verified: (pow(33) + pow(32) + pow(34)).toString(), blocks: 3, last_block_at: s.totals.last_block_at },
        "no counters of the verifier",
    );
    // Blocks of 2^33, 2^32, then 2^34 aligned past a free slot: the frontier stops at the gap.
    assert.equal(s.frontier.value, (BARINA_L0 + 3n * pow(32)).toString());
    assert.equal(s.frontier.beyond_barina, undefined, "no distance from Barina's bound");
    assert.deepEqual(Object.keys(s.frontier).sort(), ["log2", "value"]);
    assert.deepEqual(s.blocks.map((x) => x.k), [34, 32, 33], "newest first");
    const [x3, x2, x1] = s.blocks;
    for (const x of s.blocks) {
        assert.deepEqual(Object.keys(x).sort(), ["k", "numbers_per_second", "seconds", "submitted_at"], "a block's time, size and speed");
    }
    assert.deepEqual([x1.k, x1.seconds], [33, 4]);
    assert.equal(x1.numbers_per_second, 2 ** 31);
    assert.equal(x2.numbers_per_second, 2 ** 32);
    assert.equal(x3.numbers_per_second, 2 ** 33);
    // The largest peak as the page shows it: twice the T-map max_excursion,
    // as the path records give peaks.
    assert.deepEqual(s.max_excursion, { peak: (2n * BigInt(big)).toString(), peak_log2: s.max_excursion.peak_log2, n: recordN });
    close(s.max_excursion.peak_log2, Math.log2(10) * 45 + 1, "log2 of the largest peak");
    // The record is kept, but /api/stats does not list records; below the
    // frontier and above #97's peak, it is path record #98.
    assert.deepEqual([s.records, s.path_records.map((r) => [r.number, r.n])], [undefined, [[97, N97], [98, recordN]]]);
    const stored = await srv.query(`SELECT n, peak, unit_id FROM records WHERE campaign_id = ${id}`);
    assert.deepEqual(stored.map((x) => [x.n, x.peak, x.unit_id]), [[recordN, big, b2.lease.unit_id]]);
    const hosts = Object.fromEntries(s.hosts.map((h) => [h.host, [h.blocks, h.numbers, h.numbers_per_second, h.backend]]));
    assert.deepEqual(hosts, {
        [h1]: [2, (pow(33) + pow(34)).toString(), Number(pow(33) + pow(34)) / 6, "cpu"],
        [h2]: [1, pow(32).toString(), 2 ** 32, "cpu"],
    });
    const hourly = s.history.hourly;
    const daily = s.history.daily;
    assert.deepEqual([hourly.length, daily.length], [48, 90]);
    for (const series of [hourly, daily]) {
        assert.equal(sum(series.map((b) => BigInt(b.numbers))), pow(32) + pow(33) + pow(34));
        assert.equal(series.reduce((a, b) => a + b.blocks, 0), 3);
    }
    for (const b of s.blocks) {
        const hour = b.submitted_at - (b.submitted_at % 3600);
        assert.ok(hourly.find((x) => x.start === hour).blocks >= 1, "each block is in the bucket of its hour");
    }
    const w = s.speed.last_10_min;
    assert.equal(w.blocks, 3);
    // The first block started its seconds_total before it was submitted.
    const firstAt = Math.min(...s.blocks.map((b) => b.submitted_at - Math.trunc(b.seconds)));
    assert.equal(w.seconds, Math.max(1, Math.min(600, s.generated_at - firstAt)));
    close(w.numbers_per_second, Number(pow(32) + pow(33) + pow(34)) / w.seconds, "speed over the last 10 minutes");
    const day = s.speed.last_24_h;
    for (const m of s.milestones) {
        close(m.eta_seconds, (milestone(m.label) - Number(s.frontier.value)) / day.numbers_per_second, `ETA of ${m.label}`);
    }
    assert.equal(s.active_now, undefined);
    assert.equal(s.alerts_open, 0);
});

test("stats math: sliding windows, buckets, milestones and the ETA from blocks at known times", { skip }, async () => {
    const id = CAMPAIGNS.math;
    // A time in the middle of an hour, ten days ahead, so that every row is in the past.
    const T = Math.floor((nowS() + 864000) / 3600) * 3600 + 1800;
    const H = T - 1800;
    const dayAgo = T - 86400;
    // [seconds before T or a time, bits, seconds_total]
    const rows = [
        [T - 30, 32, 10],
        [T - 1000, 33, 20],
        [T - 5000, 32, 30],
        [dayAgo + 1000, 34, 40], // in the part-hour at the start of the 24-hour window
        [dayAgo + 2000, 32, 50], // in the first full hour of the window
        [dayAgo - 100, 33, 60], // just outside 24 hours, inside 48
        [T - 3 * 86400, 32, 70],
        [T - 100 * 86400, 32, 80], // outside the 90 days
    ];
    const values = rows.map(([t, bits, secs], i) => {
        const slots = 2 ** (bits - 32);
        return `(${900001 + i}, ${id}, ${100 + 8 * i}, ${bits}, ${slots}, ${t}, ${secs}, 'gpu', 'aaaaaaaaaaaaaaaa', ${1000 + i}, ${2000 + i}, ${i % 2}, '${7 + i}', '${5000 + i}')`;
    });
    await srv.sql(
        `INSERT INTO block_stats (unit_id, campaign_id, pos, bits, slots, submitted_at, seconds, backend, host, candidates, walked, overflow, max_excursion, max_excursion_n)
         VALUES ${values.join(",\n")};
         UPDATE campaigns SET frontier_pos = 5, alloc_pos = 200 WHERE id = ${id};`,
    );
    const s = (await statsOf(id, { "x-test-now": String(T) })).body;
    assert.equal(s.generated_at, T);
    const first = T - 100 * 86400 - 80;
    assert.equal(s.totals.first_block_at, undefined, "not shown; the all-time window below still starts there");
    const W = pow(32);
    const win = (name, slots, blocks, seconds) => {
        const w = s.speed[name];
        assert.deepEqual([w.blocks, w.seconds], [blocks, seconds], name);
        close(w.numbers_per_second, (slots * 2 ** 32) / seconds, name);
    };
    win("last_10_min", 1, 1, 600);
    win("last_hour", 3, 2, 3600);
    win("last_24_h", 9, 5, 86400);
    win("all_time", 13, 8, T - first);
    close(s.speed.blocks_per_day, 5, "blocks per day");
    const hourly = s.history.hourly;
    assert.deepEqual([hourly[0].start, hourly[47].start], [H - 47 * 3600, H]);
    const at = (start) => hourly.find((b) => b.start === start);
    assert.deepEqual([at(H).blocks, at(H).numbers], [2, (3n * W).toString()]);
    assert.deepEqual([at(H - 3600).blocks, at(H - 3600).numbers], [1, W.toString()]);
    assert.deepEqual([at(H - 86400).blocks, at(H - 86400).numbers], [2, (6n * W).toString()], "the part-hour block and the one before the window");
    assert.deepEqual([at(H - 82800).blocks, at(H - 82800).numbers], [1, W.toString()]);
    assert.equal(sum(hourly.map((b) => BigInt(b.numbers))), 11n * W);
    assert.equal(sum(s.history.daily.map((b) => BigInt(b.numbers))), 12n * W, "the 100-day-old block is outside the 90 days");
    assert.equal(s.history.daily[89].start, T - (T % 86400));
    // The frontier and its milestones.
    const F = BARINA_L0 + 5n * W;
    assert.equal(s.frontier.value, F.toString());
    // No frontier per 2^60, distance beyond Barina's bound or progress from 2^71.
    assert.deepEqual([s.frontier.per_2_60, s.frontier.beyond_barina, s.progress], [undefined, undefined, undefined]);
    assert.deepEqual(s.milestones.map((m) => m.label), ["2076*2^60", "2^71.1", "2^71.5", "2^72"]);
    for (const m of s.milestones) {
        close(m.eta_seconds, (milestone(m.label) - Number(F)) / s.speed.last_24_h.numbers_per_second, `ETA of ${m.label}`);
        assert.equal(m.eta_at, Math.round(T + m.eta_seconds));
    }
    assert.deepEqual([s.totals.candidates, s.totals.overflow_fallbacks], [undefined, undefined], "no counters of the verifier");
    assert.deepEqual(s.max_excursion, { peak: "28", peak_log2: Math.log2(28), n: "5007" }, "twice the largest max_excursion, 14");
    assert.equal(s.active_now, undefined);
    assert.equal((await statsOf(id, { "x-test-now": String(T + 601) })).body.speed.last_10_min.blocks, 0, "no block in the last 10 minutes");

    // A deleted row leaves every sum, and the largest is looked up again.
    await srv.sql("DELETE FROM block_stats WHERE unit_id IN (900001, 900008);");
    const d = (await statsOf(id, { "x-test-now": String(T) })).body;
    assert.deepEqual([d.speed.last_10_min.blocks, d.speed.all_time.blocks], [0, 6]);
    assert.deepEqual([d.max_excursion.peak, d.max_excursion.n], ["26", "5006"]);
    assert.equal(sum(d.history.hourly.map((b) => BigInt(b.numbers))), 10n * W);
});

test("a disagreeing re-check takes the block out of the frontier and the statistics, and an owner decision puts it back", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.recheck; // recheck_rate 1, RECHECK_EVERY 0: the owner's next fetch is the re-check
    const host = newHost();
    const u = await runOne(api, host, id, {}, (d) => (d.result.seconds_total = 3));
    let s = (await statsOf(id)).body;
    assert.deepEqual([s.totals.blocks, s.blocks.length, s.frontier.value], [1, 1, (BARINA_L0 + pow(32)).toString()]);
    const f = await fetchAs(api, host, id);
    assert.equal(f.body.lease.unit_id, u.lease.unit_id, "the same owner re-checks it, on the same machine");
    const wrong = await submitAs(api, host, u.lease.unit_id, resultFor(f.body.lease, (d) => (d.result.blocks_total = Number(String(d.result.blocks_total).replaceAll("@", "")) + 1)));
    assert.equal(wrong.status, 200, wrong.text);
    assert.equal((await srv.query(`SELECT state FROM units WHERE id = ${u.lease.unit_id}`))[0].state, "conflict");
    s = (await statsOf(id)).body;
    assert.equal(s.frontier.value, BARINA_L0.toString(), "the frontier rolls back");
    assert.equal(s.alerts_open, 1);
    assert.deepEqual([s.totals.blocks, s.blocks.length, s.max_excursion], [0, 0, null]);
    assert.deepEqual(s.hosts, [], "a machine without accepted blocks is not listed");
    assert.equal(sum(s.history.hourly.map((b) => BigInt(b.numbers))), 0n);
    // The re-check is counted, but /api/stats does not show re-check totals.
    assert.equal(s.rechecks, undefined);
    const [counts] = await srv.query(`SELECT rechecks_passed, rechecks_failed FROM campaigns WHERE id = ${id}`);
    assert.deepEqual([counts.rechecks_passed, counts.rechecks_failed], [0, 1]);
    const digest = parseExact(resultFor(u.lease)).result.result_digest;
    const dec = await api.post("/api/admin/decide", { unit_id: u.lease.unit_id, digest, note: "reran it; the first result stands" }, { authorization: `Bearer ${ADMIN_SECRET}` });
    assert.equal(dec.status, 200, dec.text);
    s = (await statsOf(id)).body;
    assert.deepEqual([s.totals.blocks, s.blocks.length, s.blocks[0].seconds], [1, 1, 3]);
    assert.equal(s.frontier.value, (BARINA_L0 + pow(32)).toString());
});

test("the stats cache: within its time the isolate copy, the Cache API and the shared D1 row answer", { skip }, async () => {
    const api = newApi();
    const id = CAMPAIGNS.cache;
    const host = newHost();
    await runOne(api, host, id);
    const on = { "x-test-stats-cache-seconds": "30" };
    const r1 = await statsOf(id, on);
    measure("stats (built, cached)", r1);
    assert.equal(r1.headers.get("x-stats-cache"), "miss");
    assert.equal(r1.headers.get("cache-control"), "public, max-age=30");
    assert.ok(r1.rowsRead > 0);
    const r2 = await statsOf(id, on);
    assert.deepEqual([r2.headers.get("x-stats-cache"), r2.rowsRead, r2.text], ["isolate", 0, r1.text]);
    const r3 = await statsOf(id, { ...on, "x-test-stats-cache-layer": "edge" });
    assert.deepEqual([r3.headers.get("x-stats-cache"), r3.rowsRead, r3.text], ["edge", 0, r1.text]);
    const r4 = await statsOf(id, { ...on, "x-test-stats-cache-layer": "d1" });
    measure("stats (from the shared D1 copy)", r4);
    assert.deepEqual([r4.headers.get("x-stats-cache"), r4.rowsRead, r4.rowsWritten, r4.text], ["d1", 1, 0, r1.text]);
    // The campaign id is read as a number: 07 and 0007 name the answer and cache entry of 7.
    for (const q of [`0${id}`, `000${id}`]) {
        const r = await newApi().get(`/api/stats?campaign=${q}`, on);
        measure(`stats ?campaign=${q} (the entry of ${id})`, r);
        assert.deepEqual([r.status, r.headers.get("x-stats-cache"), r.rowsRead, r.rowsWritten, r.text], [200, "isolate", 0, 0, r1.text], q);
    }
    await runOne(api, host, id);
    const stale = await statsOf(id, on);
    assert.deepEqual([stale.body.totals.blocks, stale.body.generated_at], [1, r1.body.generated_at], "a cached copy is served until it expires");
    const fresh = await statsOf(id);
    assert.deepEqual([fresh.headers.get("x-stats-cache"), fresh.body.totals.blocks], ["miss", 2], "without the cache time the answer is fresh");
});

test("a stats rebuild reads the shown buckets only, however many older ones the campaign keeps", { skip }, async () => {
    const id = CAMPAIGNS.buckets;
    const base = Math.floor((nowS() - 400 * 86400) / 86400) * 86400;
    // 2,000 hourly and 2,000 daily buckets, all older than the 48 hours and 90 days the page shows.
    await srv.sql(
        `WITH RECURSIVE i(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM i WHERE x < 2000)
         INSERT INTO stats_buckets (campaign_id, span, start, blocks, slots, seconds)
         SELECT ${id}, 3600, ${base} - x * 3600, 1, 1, 1 FROM i UNION ALL SELECT ${id}, 86400, ${base} - x * 86400, 1, 1, 1 FROM i;`,
    );
    const r = await statsOf(id);
    measure("stats (4,000 old buckets)", r);
    assert.ok(r.rowsRead < 100, `the stats read ${r.rowsRead} rows`);
    assert.equal(sum(r.body.history.daily.map((b) => BigInt(b.numbers))), 0n, "no old bucket is shown");
});
