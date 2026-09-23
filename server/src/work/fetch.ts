// Part of work.ts: POST /api/fetch, which leases a block to one of the owner's machines.

import { JsonObject } from "../json";
import { Db, Env, HttpError, OWNER_USER, checkHost, embargoSeconds, intVar, json, nowSeconds } from "../http";
import { Block, allowedCommits } from "../result";
import { dissolve, openBlock } from "../alloc";
import { Offer, expireDue, leaseRecheck, tryRecheck } from "../recheck";
import { SizeAsk, activeCampaign, campaignById, intField, leaseBody, requireRunnable } from "../units";
import { leaseRegular, stringChoice } from "./lease";

/**
 * A fetch looks only at this many of the lowest open units that need a
 * result, so it reads a bounded number of rows however many units lie ahead.
 * When the owner's machines already hold runs of all of them, a new block is
 * opened instead (up to the campaign's max_open).
 */
const FETCH_WINDOW = 32;
const COMMIT = /^[0-9A-Za-z._-]{1,80}$/;
/** A host's row in `hosts` is refreshed at most this often, unless what it offers changed. */
const HOST_REFRESH_SECONDS = 600;

export async function fetchWork(db: Db, env: Env, b: JsonObject): Promise<Response> {
    const host = checkHost(b["host"]);
    const commit = b["commit"];
    if (typeof commit !== "string" || !COMMIT.test(commit)) {
        throw new HttpError(400, "commit must be the binary's git commit");
    }
    const c = await activeCampaign(db, b);
    const allowed = allowedCommits(c);
    if (!allowed.includes(commit)) {
        throw new HttpError(403, `this campaign does not accept results from binary commit ${commit}; build one of the allowed commits`, {
            allowed_commits: allowed,
        });
    }
    const schema = b["digest_schema"];
    if (schema !== undefined && schema !== c.digest_schema) {
        throw new HttpError(403, `this campaign takes ${c.digest_schema} results, and this binary writes another digest schema; build one of the allowed commits`, {
            allowed_commits: allowed,
        });
    }
    const backend = stringChoice(b, "backend", ["gpu", "cpu"], "cpu") as "gpu" | "cpu";
    const recheckCpu = b["recheck_cpu"];
    if (recheckCpu !== undefined && typeof recheckCpu !== "boolean") {
        throw new HttpError(400, "recheck_cpu must be true or false");
    }
    const asked = b["bits"] === undefined ? c.min_bits : intField(b, "bits");
    const ask: SizeAsk = { asked, bits: Math.min(c.max_bits, Math.max(c.min_bits, asked)) };
    const offer: Offer = { host, backend, recheckCpu: recheckCpu === true, bits: ask.bits };
    const now = nowSeconds();
    await db.run("UPDATE runs SET state = 'expired' WHERE state = 'leased' AND expires_at <= ?1", now);
    await expireDue(db, env, c, now);
    // What this machine offers, for placing re-checks (recheck.ts).
    await db.run(
        `INSERT INTO hosts (campaign_id, user, host, backend, recheck_cpu, bits, last_seen) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
         ON CONFLICT (campaign_id, user, host) DO UPDATE SET backend = excluded.backend, recheck_cpu = excluded.recheck_cpu,
             bits = excluded.bits, last_seen = excluded.last_seen
         WHERE last_seen < excluded.last_seen - ?8 OR backend <> excluded.backend OR recheck_cpu <> excluded.recheck_cpu
            OR bits <> excluded.bits`,
        c.id,
        OWNER_USER,
        host,
        backend,
        offer.recheckCpu ? 1 : 0,
        ask.bits,
        now,
        HOST_REFRESH_SECONDS,
    );

    // A live lease of this machine is handed out again, since the worker lost
    // its local state and a second lease would only idle until expiry. A
    // re-check lease is handed out again only while it can still be submitted
    // (submit.ts).
    const held = await db.first<{ run_id: number; unit_id: number; backend: string; expires_at: number; pos: number; bits: number }>(
        `SELECT r.id AS run_id, r.unit_id, r.backend, r.expires_at, u.pos, u.bits FROM runs r JOIN units u ON u.id = r.unit_id
         WHERE r.user = ?1 AND r.state = 'leased' AND r.expires_at > ?2 AND r.host = ?3 AND u.campaign_id = ?4
           AND ((r.role = 'regular' AND u.state = 'open')
                OR (r.role = 'recheck' AND u.state = 'accepted' AND u.recheck = 'pending' AND ?2 < u.accepted_at + ?5))
         ORDER BY u.pos LIMIT 1`,
        OWNER_USER,
        now,
        host,
        c.id,
        embargoSeconds(env),
    );
    if (held !== null) {
        const unit = { id: held.unit_id, pos: held.pos, bits: held.bits };
        return json(200, { lease: leaseBody(c, unit, ask, held.backend, held.expires_at, held.run_id), redelivered: true });
    }
    const maxLeases = intVar(env.MAX_LEASES_PER_USER, 4);
    const live = await db.first<{ n: number }>(
        "SELECT COUNT(*) AS n FROM runs WHERE user = ?1 AND state = 'leased' AND expires_at > ?2 AND host = ?3",
        OWNER_USER,
        now,
        host,
    );
    if (live !== null && live.n >= maxLeases) {
        throw new HttpError(429, `this machine already holds ${live.n} leases (the limit is ${maxLeases}); finish or release one first`, {
            retry_after: 600,
        });
    }
    const lease = (unit: Block & { id: number }, used: string, runId: number): Response =>
        json(200, { lease: leaseBody(c, unit, ask, used, now + c.lease_seconds, runId) });

    // prefer_unit renews only a run of the unit that the owner's machines hold
    // released or expired, as the kind of run it is and on the backend it used.
    // For any other unit it is ignored and the answer is the usual work, so it
    // neither hands out nor reveals the re-check of a unit.
    if (b["prefer_unit"] !== undefined) {
        const want = intField(b, "prefer_unit");
        const u = await db.first<{ id: number; pos: number; bits: number }>(
            "SELECT id, pos, bits FROM units WHERE id = ?1 AND campaign_id = ?2",
            want,
            c.id,
        );
        const own =
            u === null
                ? null
                : await db.first<{ role: string; backend: string }>(
                      "SELECT role, backend FROM runs WHERE unit_id = ?1 AND user = ?2 AND state IN ('released', 'expired') ORDER BY id DESC LIMIT 1",
                      want,
                      OWNER_USER,
                  );
        if (u !== null && own !== null) {
            const renewed =
                own.role === "regular"
                    ? await leaseRegular(db, c, u.id, host, own.backend, now, true)
                    : await leaseRecheck(db, env, c, u, host, own.backend, now, true);
            if (renewed !== null) {
                return lease(u, own.backend, renewed);
            }
        }
    }
    // A re-check, when one is due.
    const recheck = await tryRecheck(db, env, c, offer, now, false);
    if (recheck !== null) {
        return lease(recheck.unit, recheck.backend, recheck.runId);
    }
    let cur = c;
    let noRoom = false;
    for (let attempt = 0; attempt < 4 && !noRoom; attempt++) {
        if (attempt > 0) {
            cur = requireRunnable(await campaignById(db, c.id));
        }
        // The inner query walks the units_need index, and LIMIT bounds the rows read.
        const cands = await db.all<{ id: number; pos: number; bits: number; results: number; assigned: number; rev: number; barred: number; kept: number }>(
            `SELECT u.id, u.pos, u.bits, u.results, u.assigned, u.rev,
                    EXISTS (SELECT 1 FROM runs r WHERE r.unit_id = u.id AND r.user = ?2
                            AND (r.state IN ('leased', 'submitted') OR r.role <> 'regular')) AS barred,
                    EXISTS (SELECT 1 FROM runs r WHERE r.unit_id = u.id AND r.state = 'released' AND r.expires_at > ?3) AS kept
             FROM (SELECT id, pos, bits, results, assigned, rev FROM units
                   WHERE campaign_id = ?1 AND state = 'open' AND assigned < wanted ORDER BY pos LIMIT ?4) u
             ORDER BY u.pos`,
            c.id,
            OWNER_USER,
            now,
            FETCH_WINDOW,
        );
        for (const u of cands) {
            if (u.barred === 1) {
                continue;
            }
            if (u.bits <= ask.bits) {
                const runId = await leaseRegular(db, c, u.id, host, backend, now, false);
                if (runId !== null) {
                    return lease(u, backend, runId);
                }
            } else if (u.results === 0 && u.assigned === 0 && u.kept === 0) {
                // Larger than asked for and given up by its last holder: free
                // its slots, so that blocks of this size can fill them.
                await dissolve(db, cur, u, now);
                break;
            }
        }
        const opened = await openBlock(db, cur, ask.bits, host, backend, now, now + c.lease_seconds);
        if (opened === null) {
            noRoom = true;
        } else if (opened !== "busy") {
            return lease({ id: opened.unitId, pos: opened.pos, bits: opened.bits }, backend, opened.runId);
        }
    }
    if (noRoom) {
        // No regular work: a waiting re-check, if one fits, whatever the pacing.
        const idle = await tryRecheck(db, env, c, offer, now, true);
        if (idle !== null) {
            return lease(idle.unit, idle.backend, idle.runId);
        }
        return json(200, {
            lease: null,
            retry_after: 600,
            reason: "no unit needs a result right now (the open units are leased, or the campaign is full)",
        });
    }
    return json(200, { lease: null, retry_after: 30, reason: "busy; retry" });
}
