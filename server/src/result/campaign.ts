// The campaign row and the blocks of its units.

export interface Campaign {
    id: number;
    name: string;
    active: number;
    l0: string;
    min_bits: number;
    max_bits: number;
    a: number;
    b: number;
    kernel: string;
    bitvectors: number;
    mod3t: number;
    partial_oee: number;
    window_chains: number;
    window_templates_d: number;
    record_threshold: string | null;
    assumption_source: string | null;
    commits: string;
    digest_schema: string;
    quorum: number;
    recheck_rate: number;
    lease_seconds: number;
    max_open: number;
    end_pos: number | null;
    created_at: number;
    frontier_pos: number;
    alloc_pos: number;
    units_open: number;
    units_accepted: number;
    slots_accepted: number;
    /** The owner's machines' random audits reported without and with failures (audit.ts). */
    audits_passed: number;
    audits_failed: number;
}

/** The block of a unit: `pos` slots of 2^min_bits from l0, 2^bits numbers. */
export interface Block {
    pos: number;
    bits: number;
}

/** Block start H*2^bits, end (H+1)*2^bits and H of a unit's block. */
export function blockRange(c: Campaign, b: Block): { h: bigint; start: bigint; end: bigint } {
    const start = BigInt(c.l0) + (BigInt(b.pos) << BigInt(c.min_bits));
    const w = 1n << BigInt(b.bits);
    return { h: start / w, start, end: start + w };
}

export const slotNumbers = (c: Campaign, slots: number | bigint): bigint => BigInt(slots) << BigInt(c.min_bits);

/** The params.unwalked_depth a result for a block of 2^bits numbers must report: M + B with M = bits - A. */
export function expectedDepth(c: Campaign, bits: number): number {
    return bits - c.a + c.b;
}

export function allowedCommits(c: Campaign): string[] {
    const v: unknown = JSON.parse(c.commits);
    return Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : [];
}
