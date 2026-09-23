// Exact BigInt arithmetic for /api/stats: the round numbers above the frontier
// that it counts down to.

import { log2Big } from "./rows";

/** floor(n^(1/k)) for n >= 0 and k >= 1: Newton's method, from above. */
function iroot(n: bigint, k: bigint): bigint {
    if (n < 2n) {
        return n;
    }
    let x = 1n << (BigInt(n.toString(2).length) / k + 1n);
    for (;;) {
        const y = ((k - 1n) * x + n / x ** (k - 1n)) / k;
        if (y >= x) {
            return x;
        }
        x = y;
    }
}

/** The smallest integer m with m >= 2^(num/den). */
function ceilPow2(num: number, den: number): bigint {
    const target = 1n << BigInt(num);
    const r = iroot(target, BigInt(den));
    return r ** BigInt(den) === target ? r : r + 1n;
}

interface Milestone {
    label: string;
    value: bigint;
}

/** The smallest 2^(k/den) above `f`, rounded up to an integer, with its label. */
function nextStep(f: bigint, den: number): Milestone {
    // The float exponent can be off by a step near a boundary; the exact values decide.
    let num = Math.floor(log2Big(f) * den) + 1;
    while (num > 1 && ceilPow2(num - 1, den) > f) {
        num--;
    }
    while (ceilPow2(num, den) <= f) {
        num++;
    }
    const e = num / den;
    return { label: `2^${Number.isInteger(e) ? String(e) : e.toFixed(1)}`, value: ceilPow2(num, den) };
}

/**
 * The next round numbers above `f`: the next multiple of 2^60, and the next
 * 2^(k/10), 2^(k/2) and 2^k, ascending, without repeats.
 */
export function milestonesAbove(f: bigint): Milestone[] {
    const m60 = ((f >> 60n) + 1n) << 60n;
    const out: Milestone[] = [{ label: `${m60 >> 60n}*2^60`, value: m60 }];
    for (const den of [10, 2, 1]) {
        const m = nextStep(f, den);
        if (!out.some((o) => o.value === m.value)) {
            out.push(m);
        }
    }
    return out.sort((a, b) => (a.value < b.value ? -1 : a.value > b.value ? 1 : 0));
}
