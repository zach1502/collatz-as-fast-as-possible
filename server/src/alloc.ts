// The block allocator. Every unit is a block of 2^bits numbers aligned to its
// own size. Below campaigns.alloc_pos the slots are covered by units and by
// free_blocks (aligned pieces that are free); from alloc_pos on everything is
// free. A fetch for 2^bits takes the lowest such block in the lookahead window
// above the frontier; if none fits there, the largest smaller free block in
// the window; if the window is full, the lowest block of the asked size beyond
// it. A free block that starts at the frontier goes first, even when it is smaller
// than the asked size. Free pieces are not merged.

import { Db, OWNER_USER } from "./http";
import { Campaign } from "./result";
import { GUARD, LOOKAHEAD_MAX_BLOCKS, isGuardFailure } from "./units";

interface Piece {
    pos: number;
    bits: number;
}

interface Placement extends Piece {
    /** The free block it is cut from, or null for a block at alloc_pos. */
    from: Piece | null;
}

const alignUp = (x: number, len: number): number => Math.ceil(x / len) * len;
const slots = (c: Campaign, bits: number): number => 2 ** (bits - c.min_bits);

/**
 * The slot by which a block of 2^bits must end for `collatz verify --block`
 * to take it (verify::validate_block): its end E = (H+1)*2^bits at most 2^96,
 * and E*3^M < 2^(125+M) with M = bits - A. The bound depends on the block's
 * end, so it is checked for every block placed, not only for the first block
 * of each size (scripts/campaign.mjs). Negative when no such block ends above
 * l0.
 */
function verifierEnd(c: Campaign, bits: number): number {
    const m = BigInt(bits - c.a);
    const lift = ((1n << (125n + m)) - 1n) / 3n ** m;
    const top = lift < 1n << 96n ? lift : 1n << 96n;
    const end = (top - BigInt(c.l0)) >> BigInt(c.min_bits);
    return end > BigInt(Number.MAX_SAFE_INTEGER) ? Number.MAX_SAFE_INTEGER : Number(end);
}

/** Aligned pieces covering [from, to), each as large as its position and the end allow. */
function pieces(c: Campaign, from: number, to: number): Piece[] {
    const out: Piece[] = [];
    let x = from;
    while (x < to) {
        let b = c.max_bits;
        while (b > c.min_bits && (x % slots(c, b) !== 0 || x + slots(c, b) > to)) {
            b--;
        }
        out.push({ pos: x, bits: b });
        x += slots(c, b);
    }
    return out;
}

/** The lowest block of 2^bits that ends by `hi` (and within the verifier's range): cut from a free block or placed at alloc_pos. */
function lowestOf(c: Campaign, free: Piece[], bits: number, hi: number): Placement | null {
    const len = slots(c, bits);
    const lim = Math.min(hi, verifierEnd(c, bits));
    const f = free.find((p) => p.bits >= bits && p.pos + len <= lim);
    const top = alignUp(c.alloc_pos, len);
    const atTop: Placement | null = top + len <= lim ? { pos: top, bits, from: null } : null;
    if (f !== undefined && (atTop === null || f.pos < atTop.pos)) {
        return { pos: f.pos, bits, from: f };
    }
    return atTop;
}

/** The largest free block smaller than 2^bits that ends by `hi` and within the verifier's range (lowest among equals). */
function largestBelow(c: Campaign, free: Piece[], bits: number, hi: number): Placement | null {
    for (let b = bits - 1; b >= c.min_bits; b--) {
        const lim = Math.min(hi, verifierEnd(c, b));
        const whole = free.find((p) => p.bits === b && p.pos + slots(c, b) <= lim);
        const top = alignUp(c.alloc_pos, slots(c, b));
        const atTop: Placement | null = top + slots(c, b) <= lim ? { pos: top, bits: b, from: null } : null;
        if (whole !== undefined && (atTop === null || whole.pos < atTop.pos)) {
            return { pos: whole.pos, bits: b, from: whole };
        }
        if (atTop !== null) {
            return atTop;
        }
    }
    return null;
}

/** Where a fetch for 2^bits goes in campaign `c` with free list `free` (see the file comment). */
function place(c: Campaign, free: Piece[], bits: number): Placement | null {
    const end = c.end_pos ?? Number.MAX_SAFE_INTEGER;
    const window = Math.min(end, c.frontier_pos + LOOKAHEAD_MAX_BLOCKS * slots(c, c.max_bits));
    // A free block that starts at the frontier and is smaller than the asked size holds
    // the frontier back, because a larger ask would skip it: lease it first.
    const held = free.find((p) => p.pos === c.frontier_pos && p.bits < bits);
    if (held !== undefined && c.frontier_pos + slots(c, held.bits) <= Math.min(window, verifierEnd(c, held.bits))) {
        return { pos: held.pos, bits: held.bits, from: held };
    }
    return lowestOf(c, free, bits, window) ?? largestBelow(c, free, bits, window) ?? lowestOf(c, free, bits, end) ?? largestBelow(c, free, bits, end);
}

interface Opened {
    unitId: number;
    pos: number;
    bits: number;
    /** The run that holds the lease (the lease id). */
    runId: number;
}

/**
 * Opens a block for a fetch of 2^bits and leases it to the owner's machine
 * `host` until `expires` in one batch: the free block it is cut from (or
 * alloc_pos) is updated under a guard, the rest of a split block and the
 * padding below an aligned block go to free_blocks, and the unit insert checks
 * that no unit overlaps it and that the campaign takes another open unit. The
 * unit wants the campaign's quorum (1) of runs. Null when the campaign has no
 * room (its end, max_open, or the end of the verifier's range: verifierEnd);
 * "busy" when a concurrent fetch changed the free space first.
 */
export async function openBlock(
    db: Db,
    c: Campaign,
    bits: number,
    host: string,
    backend: string,
    now: number,
    expires: number,
): Promise<Opened | null | "busy"> {
    if (c.units_open >= c.max_open || c.active !== 1) {
        return null;
    }
    const free = await db.all<Piece>(
        "SELECT pos, bits FROM free_blocks WHERE campaign_id = ?1 AND pos >= ?2 ORDER BY pos LIMIT 64",
        c.id,
        c.frontier_pos,
    );
    const p = place(c, free, bits);
    if (p === null) {
        return null;
    }
    const len = slots(c, p.bits);
    const stmts: D1PreparedStatement[] = [];
    const putFree = (list: Piece[]) => {
        for (const x of list) {
            stmts.push(db.prep("INSERT INTO free_blocks (campaign_id, pos, bits) VALUES (?1, ?2, ?3)", c.id, x.pos, x.bits));
        }
    };
    if (p.from !== null) {
        const f = p.from;
        stmts.push(db.prep("DELETE FROM free_blocks WHERE campaign_id = ?1 AND pos = ?2 AND bits = ?3", c.id, f.pos, f.bits), db.prep(GUARD));
        // The rest of a split block: one buddy of each size from 2^p.bits up.
        const rest: Piece[] = [];
        for (let b = p.bits; b < f.bits; b++) {
            rest.push({ pos: f.pos + slots(c, b), bits: b });
        }
        putFree(rest);
    } else {
        stmts.push(
            db.prep("UPDATE campaigns SET alloc_pos = ?1 WHERE id = ?2 AND alloc_pos = ?3", p.pos + len, c.id, c.alloc_pos),
            db.prep(GUARD),
        );
        putFree(pieces(c, c.alloc_pos, p.pos));
    }
    // Units are aligned to their power-of-two sizes, so a unit that overlaps
    // [pos, pos + len) either starts inside it or is a larger block starting
    // at one of its aligned ancestor positions: at most max_bits - bits + 1
    // index lookups on units_pos, however many units lie nearby.
    const ancestors: number[] = [];
    for (let b = p.bits + 1; b <= c.max_bits; b++) {
        const q = Math.floor(p.pos / slots(c, b)) * slots(c, b);
        if (q < p.pos && ancestors[ancestors.length - 1] !== q) {
            ancestors.push(q);
        }
    }
    stmts.push(
        db.prep(
            `INSERT INTO units (campaign_id, pos, bits, len, wanted, created_at)
             SELECT id, ?2, ?3, ?4, ?7, ?5 FROM campaigns
             WHERE id = ?1 AND active = 1 AND units_open < max_open
               AND NOT EXISTS (SELECT 1 FROM units WHERE campaign_id = ?1 AND pos >= ?2 AND pos < ?2 + ?4)
               AND NOT EXISTS (SELECT 1 FROM units WHERE campaign_id = ?1 AND pos IN (SELECT value FROM json_each(?6)) AND pos + len > ?2)
             RETURNING id`,
            c.id,
            p.pos,
            p.bits,
            len,
            now,
            JSON.stringify(ancestors),
            c.quorum,
        ),
        db.prep(GUARD),
        db.prep(
            `INSERT INTO runs (unit_id, user, host, role, state, backend, leased_at, expires_at)
             SELECT id, ?3, ?4, 'regular', 'leased', ?5, ?6, ?7 FROM units WHERE campaign_id = ?1 AND pos = ?2
             RETURNING id`,
            c.id,
            p.pos,
            OWNER_USER,
            host,
            backend,
            now,
            expires,
        ),
    );
    let rs: D1Result[];
    try {
        rs = await db.batch(stmts);
    } catch (e) {
        if (isGuardFailure(e)) {
            return "busy";
        }
        throw e;
    }
    const inserted = rs[rs.length - 3]?.results as Array<{ id: number }> | undefined;
    const run = rs[rs.length - 1]?.results as Array<{ id: number }> | undefined;
    const unitId = inserted?.[0]?.id;
    const runId = run?.[0]?.id;
    return unitId === undefined || runId === undefined ? "busy" : { unitId, pos: p.pos, bits: p.bits, runId };
}

/**
 * Dissolves open unit `u` into a free block, so that its slots can be handed
 * out in other sizes: only a unit without results, without a live lease and
 * without a released lease whose time has not run out (a paused worker comes
 * back for it), and unchanged since it was read (rev). False when that no
 * longer holds.
 */
export async function dissolve(db: Db, c: Campaign, u: { id: number; pos: number; bits: number; rev: number }, now: number): Promise<boolean> {
    try {
        await db.batch([
            db.prep(
                "DELETE FROM runs WHERE unit_id = ?1 AND (state = 'expired' OR (state = 'released' AND expires_at <= ?2))",
                u.id,
                now,
            ),
            db.prep(
                `DELETE FROM units WHERE id = ?1 AND state = 'open' AND results = 0 AND assigned = 0 AND rev = ?2
                   AND NOT EXISTS (SELECT 1 FROM runs WHERE unit_id = ?1)`,
                u.id,
                u.rev,
            ),
            db.prep(GUARD),
            db.prep("INSERT INTO free_blocks (campaign_id, pos, bits) VALUES (?1, ?2, ?3)", c.id, u.pos, u.bits),
        ]);
        return true;
    } catch (e) {
        if (isGuardFailure(e)) {
            return false;
        }
        throw e;
    }
}
