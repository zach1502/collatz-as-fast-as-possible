// Part of units.ts: campaign lookup and the checks a campaign must pass before
// its blocks are handed out, integer request fields, the answer to a result on
// a lapsed lease, and the body of a lease.

import { JsonNumber, JsonObject } from "../json";
import { Db, HttpError } from "../http";
import { Block, Campaign, DIGEST_SCHEMA, blockRange } from "../result";

/** The lookahead window above the frontier, in blocks of the campaign's largest size. */
export const LOOKAHEAD_MAX_BLOCKS = 2;
/** A host counts as active in a campaign for this long after its last fetch. */
export const ACTIVE_SECONDS = 86400;

export function intField(b: JsonObject, key: string): number {
    const v = b[key];
    if (!(v instanceof JsonNumber) || !/^(0|[1-9][0-9]{0,14})$/.test(v.raw)) {
        throw new HttpError(400, `${key} must be a non-negative integer`);
    }
    return Number(v.raw);
}

/**
 * The body's `lease_id`: the id /api/fetch gave the lease (lease.lease_id),
 * which submit and release must send. A worker built before leases had ids
 * sends none; the answer says to update it.
 */
export function leaseIdField(b: JsonObject): number {
    if (b["lease_id"] === undefined) {
        throw new HttpError(400, "lease_id is missing: send the lease_id of the lease from /api/fetch (update collatz worker to a build that sends it)");
    }
    return intField(b, "lease_id");
}

/**
 * The answer to a result on a lease that was handed back or ran out. Only a
 * live lease takes a result, so a lease cannot be cashed in later (for
 * example with a copy of a published result). Fetching the unit again
 * (prefer_unit) renews it while the unit still takes the run.
 */
export function lapsedError(unitId: number, state: "released" | "expired"): HttpError {
    return new HttpError(
        409,
        `your lease of unit ${unitId} ${state === "released" ? "was released" : "expired"}; fetch the unit again (prefer_unit) to submit`,
        { lease_state: state },
    );
}

export async function campaignById(db: Db, id: number): Promise<Campaign> {
    const c = await db.first<Campaign>("SELECT * FROM campaigns WHERE id = ?1", id);
    if (c === null) {
        throw new HttpError(404, `no campaign ${id}`);
    }
    return c;
}

/**
 * Refuses a campaign whose blocks this server cannot hand out or check, with
 * a 500 so that workers back off and retry. l0 must be a positive multiple of
 * 2^max_bits, so that a block aligned to its size relative to l0 is aligned in
 * absolute terms (`verify --block H --K bits`). The sizes and sieve parameters
 * must be ones the verifier runs, one result must accept a block (quorum 1),
 * and digest_schema must be the one result.ts implements. scripts/campaign.mjs
 * writes only such rows.
 */
export function requireRunnable(c: Campaign): Campaign {
    const bad = (why: string): never => {
        throw new HttpError(500, `campaign ${c.id} is misconfigured: ${why}`);
    };
    if (c.min_bits < 32 || c.min_bits > c.max_bits || c.max_bits > 64 || c.max_bits - c.min_bits > 30) {
        bad(`block sizes 2^${c.min_bits}..2^${c.max_bits} are outside 2^32..2^64`);
    }
    const l0 = BigInt(c.l0);
    if (l0 <= 0n || l0 % (1n << BigInt(c.max_bits)) !== 0n) {
        bad(`l0 is not a positive multiple of 2^${c.max_bits}`);
    }
    if (c.a < 1 || c.a > 14 || c.a >= c.min_bits || c.max_bits - c.a > 76 || c.b < 2 || c.b > 32) {
        bad(`A=${c.a}, B=${c.b} do not fit every block size (1 <= A <= 14, A < min_bits, max_bits - A <= 76, 2 <= B <= 32)`);
    }
    if (c.quorum !== 1) {
        bad(`quorum ${c.quorum} is not 1 (one result accepts a block)`);
    }
    if (c.digest_schema !== DIGEST_SCHEMA) {
        throw new HttpError(500, `campaign ${c.id} records digest schema ${c.digest_schema}; this server checks ${DIGEST_SCHEMA}`);
    }
    return c;
}

/** The campaign named by `campaign` in the body, else the oldest active one. */
export async function activeCampaign(db: Db, b: JsonObject): Promise<Campaign> {
    if (b["campaign"] !== undefined) {
        const c = await campaignById(db, intField(b, "campaign"));
        if (c.active !== 1) {
            throw new HttpError(410, `campaign ${c.id} is closed`);
        }
        return requireRunnable(c);
    }
    const c = await db.first<Campaign>("SELECT * FROM campaigns WHERE active = 1 ORDER BY id LIMIT 1");
    if (c === null) {
        throw new HttpError(503, "no active campaign", { retry_after: 3600 });
    }
    return requireRunnable(c);
}

/** The block size a fetch asked for (`bits`, default min_bits), and that size brought into the campaign's range. */
export interface SizeAsk {
    asked: number;
    bits: number;
}

/**
 * The lease of block `unit`. Every lease has the same fields whatever kind
 * of run it is (a worker is not told that a lease is a re-check): `lease_id`
 * is the id of the run (runs.id) that holds the lease, which submit and
 * release must name; `note` says why the block is smaller than asked for, or
 * that the size was brought into the campaign's range, and is null
 * otherwise. The lease lasts the campaign's lease_seconds.
 */
export function leaseBody(
    c: Campaign,
    unit: Block & { id: number },
    ask: SizeAsk,
    backend: string,
    expires: number,
    leaseId: number,
): Record<string, unknown> {
    const { h, start, end } = blockRange(c, unit);
    let note: string | null = null;
    if (unit.bits < ask.bits) {
        note = `a free block of 2^${unit.bits} numbers near the frontier is smaller than the 2^${ask.bits} asked; this unit is that block`;
    } else if (ask.asked !== ask.bits) {
        note = `this campaign hands out blocks of 2^${c.min_bits} to 2^${c.max_bits} numbers; this unit is a block of 2^${unit.bits}`;
    }
    return {
        unit_id: unit.id,
        lease_id: leaseId,
        campaign_id: c.id,
        h: h.toString(),
        k: unit.bits,
        l: start.toString(),
        end: end.toString(),
        a: c.a,
        b: c.b,
        kernel: c.kernel,
        bitvectors: c.bitvectors,
        mod3t: c.mod3t,
        partial_oee: c.partial_oee !== 0,
        window_chains: c.window_chains,
        window_templates_d: c.window_templates_d,
        record_threshold: c.record_threshold,
        assumption_source: c.assumption_source,
        backend,
        lease_expires: expires,
        note,
    };
}
