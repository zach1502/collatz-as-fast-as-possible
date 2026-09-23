// Parts of the stats document: a block as the page shows it, the largest peak
// and the milestones above the frontier.

import { milestonesAbove } from "./milestones";
import { BlockRow, TotalsRow, log2Big, rate, round3 } from "./rows";

/** Block `b`: when it was accepted, its size, time and speed. */
export function blockView(b: BlockRow): Record<string, unknown> {
    return {
        submitted_at: b.submitted_at,
        k: b.bits,
        seconds: round3(b.seconds),
        numbers_per_second: rate(1n << BigInt(b.bits), b.seconds),
    };
}

/**
 * The largest value on a walked orbit, from the totals `t`, as the page and
 * the path records give peaks: in the 3n+1 map, twice the verifier's
 * max_excursion (T-map), with its starting number.
 */
export function peakView(t: TotalsRow | null): { peak: string; peak_log2: number; n: string | null } | null {
    if (t?.max_excursion === null || t?.max_excursion === undefined) {
        return null;
    }
    const peak = 2n * BigInt(t.max_excursion);
    return { peak: peak.toString(), peak_log2: log2Big(peak), n: t.max_excursion_n };
}

/**
 * The milestones above `frontier`, each with its label and, at `perSecond`
 * numbers per second from `now`, the seconds until the frontier reaches it
 * and that time; both are null without a positive speed.
 */
export function milestoneViews(frontier: bigint, perSecond: number | null, now: number): Array<Record<string, unknown>> {
    return milestonesAbove(frontier).map((m) => {
        const remaining = m.value - frontier;
        const eta = perSecond !== null && perSecond > 0 ? Number(remaining) / perSecond : null;
        return {
            label: m.label,
            eta_seconds: eta,
            eta_at: eta === null ? null : Math.round(now + eta),
        };
    });
}
