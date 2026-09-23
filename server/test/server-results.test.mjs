// npm test: result validation, the stored document of an accepted unit,
// digest schemas and campaign starts. helpers-server.mjs starts the Worker and
// holds the shared helpers.

import assert from "node:assert/strict";
import { test } from "node:test";

import { CAMPAIGNS, TOKEN, cost, fetchFor, machines, off, releaseFor, resultFor, skip, srv, submitFor, submitText } from "./helpers-server.mjs";
import { client, parseExact, stringifyExact } from "./helpers.mjs";

test("submit: rejects bad digests, wrong parameters and missing leases", { skip }, async () => {
    const api = client(srv.url, "10.1.0.4");
    const id = CAMPAIGNS.validate;
    const m = machines();
    const lx = (await fetchFor(api, m, id)).body.lease;
    const idx = off(lx);
    const expect422 = async (doc, what) => {
        const r = await submitFor(api, m, lx.unit_id, doc);
        assert.equal(r.status, 422, `${what}: ${r.text}`);
        assert.match(r.body.error, /result rejected/);
        return r.body.error;
    };
    const tampered = parseExact(resultFor(idx));
    tampered.result.result_digest = "0".repeat(64);
    assert.match(await expect422(stringifyExact(tampered), "bad digest"), /does not match the result fields/);
    const blocks = parseExact(resultFor(idx));
    blocks.result.blocks_total = "@@123456789012345678@@"; // field changed, digest not
    assert.match(await expect422(stringifyExact(blocks), "edited field"), /does not match the result fields/);
    assert.match(await expect422(resultFor(idx, (d) => (d.params.A = 9)), "A"), /params\.A/);
    assert.match(await expect422(resultFor(idx, (d) => (d.params.B = 25)), "B"), /params\.B/);
    assert.match(await expect422(resultFor(idx, (d) => (d.params.kernel = "other")), "kernel"), /params\.kernel/);
    assert.match(await expect422(resultFor(idx, (d) => (d.params.N = 33)), "K"), /params\.N/);
    assert.match(await expect422(resultFor(idx + 1), "another unit's result"), /block\.h/);
    assert.match(
        await expect422(resultFor(idx, (d) => (d.params.assumes_below = "1")), "assumes_below"),
        /params\.assumes_below/,
    );
    assert.match(await expect422(resultFor(idx, (d) => (d.commit = "0123abc")), "commit"), /not accepted/);
    assert.match(
        await expect422(resultFor(idx, (d) => (d.result.max_excursion = "12a")), "decimal"),
        /max_excursion is not a canonical decimal/,
    );
    assert.match(
        await expect422(resultFor(idx, (d) => (d.result.candidates_checked = "@@18446744073709551616@@")), "u64"),
        /candidates_checked is not an integer/,
    );
    assert.match(await expect422(resultFor(idx, (d) => (d.params.case_k = 2)), "slice"), /case/);
    assert.match(await expect422(resultFor(idx, (d) => (d.result.max_excursion_n = "5")), "argmax"), /outside the unit/);
    assert.match(await expect422(resultFor(idx, (d) => (d.preflight.candidates = 1)), "preflight"), /preflight/);
    assert.match(await expect422(resultFor(idx, (d) => (d.result.records = [["1", "2"]])), "records"), /records/);
    assert.match(await expect422('{"schema": "collatz-verify-json/1"}', "empty"), /missing commit/);

    const big = submitText(m, lx.unit_id, `{"pad": "${"x".repeat(300000)}"}`);
    assert.equal((await api.post("/api/submit", big)).status, 413);
    const over = parseExact(resultFor(idx));
    over.pad = "x".repeat(270000); // a valid result plus 270 KB: over the 256 KB cap of /api/submit
    assert.equal((await submitFor(api, m, lx.unit_id, stringifyExact(over))).status, 413);
    assert.match(
        await expect422(resultFor(idx, (d) => (d.result.result_digest_schema = "collatz-result-digest/1")), "schema"),
        /result_digest_schema/,
    );

    const body = (unitId, lease) => `{"token":${JSON.stringify(TOKEN)},"host":"${m.hosts[0]}","unit_id":${unitId}${lease},"result_json":${resultFor(idx)}}`;
    const noLease = await api.post("/api/submit", body(lx.unit_id, ""));
    assert.equal(noLease.status, 400, `a worker that sends no lease_id is refused: ${noLease.text}`);
    assert.match(noLease.body.error, /lease_id is missing/);
    const none = await api.post("/api/submit", body(999999, `,"lease_id":${lx.lease_id}`));
    assert.equal(none.status, 409, "no lease, no submit");
    assert.match(none.body.error, /no lease for unit 999999/);
    const [stored] = await srv.query(`SELECT results FROM units WHERE id = ${lx.unit_id}`);
    assert.equal(stored.results, 0, "a refused result is not stored");

    const ok = await submitFor(api, m, lx.unit_id, resultFor(idx));
    assert.equal(ok.status, 200, ok.text);
    assert.equal(ok.body.state, "accepted");
});

test("stored documents: an accepted unit keeps one canonical copy of its result", { skip }, async () => {
    const api = client(srv.url, "10.1.0.13");
    const id = CAMPAIGNS.stored;
    const m = machines();
    const la = (await fetchFor(api, m, id)).body.lease;
    const doc = resultFor(off(la), (d) => {
        d.unknown_top = "dropped";
        d.result.unknown_counter = 7;
        d.machine.host = "secret-host";
    });
    assert.equal((await submitFor(api, m, la.unit_id, doc)).body.state, "accepted");
    const digest = parseExact(doc).result.result_digest;
    const pow = String(parseExact(doc).result.pow_checksum).replaceAll("@", "");
    // The canonical document only: no unknown keys and no machine block.
    const [stored] = await srv.query(`SELECT result_json FROM unit_results WHERE unit_id = ${la.unit_id}`);
    const kept = JSON.parse(stored.result_json);
    assert.equal(kept.unknown_top, undefined);
    assert.equal(kept.machine, undefined);
    assert.equal(kept.result.unknown_counter, undefined);
    assert.equal(kept.result.result_digest, digest);
    assert.deepEqual(Object.keys(kept), ["schema", "commit", "backend", "block", "params", "result", "preflight"]);
    assert.ok(stored.result_json.includes(`"pow_checksum":${pow}`), "u64 text kept exactly");
    const rows = await srv.query(`SELECT COUNT(*) AS n FROM runs WHERE unit_id = ${la.unit_id} AND result_json IS NOT NULL`);
    assert.equal(rows[0].n, 0, "the run keeps no second copy");
    const page = (await api.get(`/api/stats?campaign=${id}`)).text;
    assert.ok(!page.includes(digest) && !page.includes("secret-host"), "/api/stats shows no digest and no machine block");
});

test("digest schema: campaigns record it and other schemas are refused", { skip }, async () => {
    const api = client(srv.url, "10.1.0.15");
    const m = machines();
    const [c] = await srv.query(`SELECT digest_schema FROM campaigns WHERE id = ${CAMPAIGNS.basic}`);
    assert.equal(c.digest_schema, "collatz-result-digest/2");
    const old = await fetchFor(api, m, CAMPAIGNS.oldSchema);
    assert.equal(old.status, 500);
    assert.match(old.body.error, /digest schema collatz-result-digest\/1/);
    const client1 = await fetchFor(api, m, CAMPAIGNS.basic, { digest_schema: "collatz-result-digest/1" });
    assert.equal(client1.status, 403);
    const lease = await fetchFor(api, m, CAMPAIGNS.basic, { digest_schema: "collatz-result-digest/2" });
    assert.equal(lease.status, 200, lease.text);
    await releaseFor(api, m, lease.body.lease.unit_id);
});

test("campaigns whose l0 is not a multiple of 2^K hand out no work", { skip }, async () => {
    const api = client(srv.url, "10.1.0.16");
    const r = await fetchFor(api, machines(), CAMPAIGNS.badL0);
    assert.equal(r.status, 500);
    assert.match(r.body.error, /not a positive multiple of 2\^32/);
});

test("submit: a result over 32 KB (a block with 1000 records) is taken; the other requests keep the 32 KB cap", { skip }, async () => {
    const api = client(srv.url, "10.1.0.17");
    const m = machines();
    const lx = (await fetchFor(api, m, CAMPAIGNS.bigResult)).body.lease;
    const doc = resultFor(off(lx), (d) => {
        const L = BigInt(d.block.L);
        d.params.record_threshold = "1";
        d.result.records = Array.from({ length: 1000 }, (_, i) => [(L + 2n * BigInt(i) + 1n).toString(), "123456789012345678901234567890"]);
    });
    assert.ok(doc.length > 40000, `the result has ${doc.length} bytes`);
    const s = await submitFor(api, m, lx.unit_id, doc);
    assert.equal(s.status, 200, s.text.slice(0, 300));
    assert.equal(s.body.state, "accepted");
    cost.push({ what: "submit (1000 records, over 32 KB)", read: s.rowsRead, written: s.rowsWritten });
    const [n] = await srv.query(`SELECT COUNT(*) AS n FROM records WHERE campaign_id = ${CAMPAIGNS.bigResult} AND unit_id = ${lx.unit_id}`);
    assert.equal(n.n, 1000, "every record is stored");
    const padded = JSON.stringify({ token: TOKEN, host: m.hosts[0], unit_id: lx.unit_id, lease_id: lx.lease_id, pad: "x".repeat(33000) });
    assert.equal((await api.post("/api/release", padded)).status, 413, "a release over 32 KB is refused");
});
