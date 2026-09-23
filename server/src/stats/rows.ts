// The time windows and list lengths of the stats document, the rows its
// queries read and the helpers that format their values.

export const HOUR = 3600;
export const DAY = 86400;
export const HOURS = 48;
export const DAYS = 90;
/** Sliding windows of the speed, in seconds, by their JSON names. */
export const WINDOWS: Array<[string, number]> = [
    ["last_10_min", 600],
    ["last_hour", HOUR],
    ["last_24_h", DAY],
];
export const LAST_BLOCKS = 20;
export const MAX_HOSTS = 50;

/** Bit length plus fraction: log2 of a positive BigInt to double precision. */
export function log2Big(x: bigint): number {
    if (x <= 0n) {
        return 0;
    }
    const s = x.toString(2);
    const top = s.slice(0, 53);
    return Math.log2(parseInt(top, 2)) + (s.length - top.length);
}

export interface TotalsRow {
    blocks: number;
    slots: number;
    first_at: number | null;
    last_at: number | null;
    max_excursion: string | null;
    max_excursion_n: string | null;
}

export interface BlockRow {
    bits: number;
    submitted_at: number;
    seconds: number;
}

export interface BucketRow {
    span: number;
    start: number;
    blocks: number;
    slots: number;
}

export const BLOCK_COLS = "bits, submitted_at, seconds";

export const round3 = (x: number): number => Math.round(x * 1000) / 1000;
export const rate = (numbers: bigint, seconds: number): number | null => (seconds > 0 ? Number(numbers) / seconds : null);
