// Step 3 single-candidate 16-step block finisher, finish_block16.

#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "step3_internal.hpp"

#include <cstdint>

namespace collatz::step3 {

using namespace detail;

BlockOutcome finish_block16(u128 n, u128 m, u128 max_threshold) noexcept {
    BlockOutcome o;
    o.max_seen = m;
    // The trivial cycle {1, 2} is the induction base: n == 1 has nothing strictly
    // below it, and n == 2 reaches 1 (< 2) after a single step inside the first
    // block. Because 2 is a fixed point of the 16-step block map (T^16(2) == 2),
    // a block starting at 2 lands on 2 forever, so the block-granular loop could
    // never observe the mid-block descent and would spin. Both are discharged
    // by the base case with 0 blocks (their descent is trivial and certain).
    if (n <= 2) {
        o.v_end = n; // base case: the landing value is n itself (blocks == 0).
        return o;
    }

    const BlockTable16& tb = block_table16();
    u128 v = m;
    u128 maxv = m;
    std::uint64_t blocks = 0;

    while (v >= n) {
        // Record-contender test. `bar` is the value this block's peak must reach
        // to matter: the larger of the running exact max and the caller's
        // threshold. A block's peak is < (v + 1) * (3/2)^16 < (v + 1) << 10, so
        // v < bar>>10 proves the peak < bar, and the block needs no exact work.
        // The verifier passes a high threshold, so almost every block passes
        // this one compare.
        const u128 bar = maxv > max_threshold ? maxv : max_threshold;
        if (v >= (bar >> 10)) {
            bool ub_ovf = false;
            const u128 ub = block_peak_ub(v, ub_ovf);
            if (ub_ovf || ub >= bar) {
                if (!block_exact_peak(v, maxv)) {
                    o.blocks = blocks;
                    o.max_seen = maxv;
                    o.overflow = true;
                    return o;
                }
            }
        }

        // One 16-step block from the table.
        const std::uint32_t packed = tb[static_cast<std::uint32_t>(lo64(v) & 0xFFFFu)];
        const unsigned f = packed & kFMask;
        const u128 c = static_cast<u128>(packed >> 5u);
        u128 next = 0;
        if (!block_landing(v, f, c, next)) {
            o.blocks = blocks;
            o.max_seen = maxv;
            o.overflow = true;
            return o;
        }
        v = next;
        ++blocks;
    }

    o.blocks = blocks;
    o.max_seen = maxv;
    o.v_end = v; // the first block-boundary value < n.
    return o;
}

} // namespace collatz::step3
