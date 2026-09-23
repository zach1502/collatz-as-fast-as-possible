// The speed of a campaign over the sliding windows and all time, and its
// hourly and daily history, from the rows the stats document reads.

import { Campaign, slotNumbers } from "../result";
import { BucketRow, DAY, DAYS, HOUR, HOURS, TotalsRow, WINDOWS, rate } from "./rows";

/**
 * The speed of campaign `c` at `now` over each window of WINDOWS and over all
 * time, from its totals `t`. The speed over each window counts the blocks
 * accepted in it; a window longer than the time since the first block started
 * is cut to that time. A window of at most an hour counts the blocks of
 * `recentRows`; a longer one counts `partialRows`, the blocks of the
 * part-hour before `fullFrom`, and the hourly buckets that start at or after
 * `fullFrom`.
 */
export function speedWindows(
    c: Campaign,
    t: TotalsRow | null,
    now: number,
    fullFrom: number,
    recentRows: Array<{ submitted_at: number; slots: number }>,
    partialRows: Array<{ slots: number }>,
    bucketRows: BucketRow[],
): Record<string, unknown> {
    const numbersOf = (slots: number): bigint => slotNumbers(c, slots);
    const since = t?.first_at ?? null;
    const span = (w: number): number => (since === null ? 0 : Math.max(1, Math.min(w, now - since)));
    const speed: Record<string, unknown> = {};
    for (const [name, w] of WINDOWS) {
        let slots = 0;
        let blocks = 0;
        if (w <= HOUR) {
            for (const r of recentRows) {
                if (r.submitted_at > now - w) {
                    slots += r.slots;
                    blocks++;
                }
            }
        } else {
            for (const r of partialRows) {
                slots += r.slots;
                blocks++;
            }
            for (const b of bucketRows) {
                if (b.span === HOUR && b.start >= fullFrom) {
                    slots += b.slots;
                    blocks += b.blocks;
                }
            }
        }
        const s = span(w);
        speed[name] = { seconds: s, blocks, numbers_per_second: rate(numbersOf(slots), s) };
    }
    const allSpan = since === null ? 0 : Math.max(1, now - since);
    const allNumbers = numbersOf(t?.slots ?? 0);
    speed["all_time"] = { seconds: allSpan, blocks: t?.blocks ?? 0, numbers_per_second: rate(allNumbers, allSpan) };
    return speed;
}

interface HistoryPoint {
    start: number;
    blocks: number;
    numbers: string;
}

/**
 * The HOURS hourly buckets of campaign `c` from `firstHour` and its DAYS daily
 * buckets from `firstDay`, read from `bucketRows`; a bucket without a row
 * counts nothing.
 */
export function bucketHistory(c: Campaign, bucketRows: BucketRow[], firstHour: number, firstDay: number): { hourly: HistoryPoint[]; daily: HistoryPoint[] } {
    const numbersOf = (slots: number): bigint => slotNumbers(c, slots);
    const series = (s: number, first: number, n: number) => {
        const byStart = new Map(bucketRows.filter((b) => b.span === s).map((b) => [b.start, b]));
        return Array.from({ length: n }, (_, i) => {
            const b = byStart.get(first + i * s);
            return { start: first + i * s, blocks: b?.blocks ?? 0, numbers: numbersOf(b?.slots ?? 0).toString() };
        });
    };
    return { hourly: series(HOUR, firstHour, HOURS), daily: series(DAY, firstDay, DAYS) };
}
