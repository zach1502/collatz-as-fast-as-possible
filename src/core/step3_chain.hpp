#pragma once

// The block chain of the slot driver, its 16-step stepper block_step, and the
// multi-chain u64 fast-lane spin (spin_fastlane).

#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "step3_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace collatz::step3::detail {

/// One in-flight block chain of the slot driver. `active` stays true while the
/// chain is above n (and has not overflowed); on finish it writes its result.
struct BlockChain {
    u128 v = 0;
    u128 maxv = 0;
    u128 n = 0;
    u128 threshold = 0;
    std::uint64_t blocks = 0;
    bool active = false;
    BlockOutcome* out = nullptr;
};

/// Advances one block chain by one 16-step block, exactly as finish_block16
/// does. Chains are independent, so two calls back to back overlap in the
/// out-of-order window.
static inline void block_step(BlockChain& c, const BlockTable16& tb) noexcept {
    if (!c.active) {
        return;
    }
    if (c.v < c.n) {
        c.out->blocks = c.blocks;
        c.out->max_seen = c.maxv;
        c.out->v_end = c.v; // the first block-boundary value < n.
        c.active = false;
        return;
    }
    const u128 bar = c.maxv > c.threshold ? c.maxv : c.threshold;
    // u64 fast lane, taken exactly when the u128 path below would only land the
    // block:
    //   (i)  v < 2^54: the landing (v>>16)*3^f + c fits u64 (see kU64FastBar),
    //        one 64x32 madd instead of a u128 multiply;
    //   (ii) v < bar>>10: the block's peak (< (v+1)*(3/2)^16 < (v+1)<<10) cannot
    //        reach the record bar, so the u128 path would take neither
    //        block_peak_ub nor block_exact_peak.
    // `next` equals block_landing's fast branch bit for bit (both are
    // (v>>16)*kPow3Low[f] + c < 2^64) and is stored full-width, so a block that
    // climbs back above 2^54 takes the u128 path in the next call.
    if (c.v < kU64FastBar && c.v < (bar >> 10)) {
        // Single-chain register spin. In the lane the block peak stays below the
        // bar, so maxv, n and threshold do not change and `bar>>10` is
        // loop-invariant; only v and the block count change. They stay in
        // registers and the struct is written back once, instead of a BlockChain
        // load and store per block, and the block count and max_seen are
        // bit-identical. Once the predicate fails, the next block_step call takes
        // the retire (v < n) or u128-promote (v >= 2^54 or v >= bar>>10) path.
        // All state is u64: the retire check above gives c.v >= c.n and the entry
        // gate gives c.v < 2^54, so n <= v < 2^54 and n64 is exact. The continue
        // condition `v >= n & v < 2^54 & v < fastcap` is exactly `n <= v < hi`
        // with hi = min(2^54, fastcap) <= 2^54. Since n <= hi, it folds into the
        // one unsigned compare `(v - n64) < span`, span = hi - n64: for v >= n64 it
        // is v < hi, and for v < n64 the subtraction wraps above span, so the loop
        // exits.
        const u128 fastcap = bar >> 10; // maxv invariant in-lane -> loop-invariant
        const auto n64 = static_cast<std::uint64_t>(lo64(c.n));
        const auto fastcap64 =
            hi64(fastcap) != 0 ? lo64(kU64FastBar) : static_cast<std::uint64_t>(lo64(fastcap));
        const std::uint64_t hi = fastcap64 < lo64(kU64FastBar) ? fastcap64 : lo64(kU64FastBar);
        const std::uint64_t span = hi - n64; // n64 <= c.v < hi, so span >= 1
        auto v = static_cast<std::uint64_t>(lo64(c.v));
        std::uint64_t blk = c.blocks;
        do {
            const std::uint32_t packed = tb[v & 0xFFFFu];
            const unsigned f = packed & kFMask;
            const auto cc = static_cast<std::uint64_t>(packed >> 5u);
            v = (v >> kBlockStep) * static_cast<std::uint64_t>(kPow3Low[f]) + cc;
            ++blk;
        } while (v - n64 < span);
        c.v = static_cast<u128>(v);
        c.blocks = blk;
        return;
    }
    if (c.v >= (bar >> 10)) {
        bool ub_ovf = false;
        const u128 ub = block_peak_ub(c.v, ub_ovf);
        if (ub_ovf || ub >= bar) {
            if (!block_exact_peak(c.v, c.maxv)) {
                c.out->blocks = c.blocks;
                c.out->max_seen = c.maxv;
                c.out->overflow = true;
                c.active = false;
                return;
            }
        }
    }
    const std::uint32_t packed = tb[static_cast<std::uint32_t>(lo64(c.v) & 0xFFFFu)];
    const unsigned f = packed & kFMask;
    const auto cc = static_cast<u128>(packed >> 5u);
    u128 next = 0;
    if (!block_landing(c.v, f, cc, next)) {
        c.out->blocks = c.blocks;
        c.out->max_seen = c.maxv;
        c.out->overflow = true;
        c.active = false;
        return;
    }
    c.v = next;
    ++c.blocks;
}

/// Register-only W-chain fast-lane spin (2 <= W <= kWmax). It drains W
/// independent block chains that are all in the u64 fast lane (the caller
/// guarantees each passes block_step's fast-lane entry gate: active, v >= n,
/// v < 2^54, v < bar>>10). Each chain's per-block arithmetic and exit predicate
/// are those of the single-chain spin inside block_step, byte for byte; the W
/// latency-bound madd sequences run interleaved in one register loop, so an
/// out-of-order core overlaps their table load-use and multiply latency.
///
/// The state (v, n64, span, blk, live) of all W chains stays in registers, with
/// no spill per block. Each chain advances one block per iteration until it
/// leaves its lane (`live[j] = false`), and the loop runs until all W have left,
/// so each chain takes exactly the blocks the single-chain spin would (same
/// v-sequence, same count). On exit each chain is written back, out of its fast
/// lane and still active; the driver's next block_step does the retire, promote
/// or exact-max transition. `blocks`, `v_end` (via c.v) and `max_seen` are
/// bit-identical.
template <std::size_t W>
static inline void spin_fastlane(std::array<BlockChain*, W> cs, const BlockTable16& tb) noexcept {
    static_assert(W >= 2, "spin_fastlane overlaps >= 2 chains");
    static constexpr std::array<std::uint32_t, kBlockStep + 1> kP = kPow3Low;
    static constexpr std::uint64_t kMask = 0xFFFFu;
    // (3/2)^16 < 2^10, so v < bar>>kSpanShift proves the block peak stays below
    // the record bar, as in block_step.
    static constexpr unsigned kSpanShift = 10;
    const std::uint32_t* const T = tb.data();
    std::uint64_t v[W];
    std::uint64_t n64[W];
    std::uint64_t span[W];
    std::uint64_t blk[W];
    bool live[W];
    std::size_t nlive = W;
    for (std::size_t j = 0; j < W; ++j) {
        const BlockChain& c = *cs[j];
        // bar / span exactly as block_step's fast lane: maxv/n/threshold are
        // invariant while a chain stays in the u64 lane, so span is loop-constant.
        const u128 bar = c.maxv > c.threshold ? c.maxv : c.threshold;
        const u128 fastcap = bar >> kSpanShift;
        const auto nn = static_cast<std::uint64_t>(lo64(c.n));
        const std::uint64_t fastcap64 =
            hi64(fastcap) != 0 ? lo64(kU64FastBar) : static_cast<std::uint64_t>(lo64(fastcap));
        const std::uint64_t hi = fastcap64 < lo64(kU64FastBar) ? fastcap64 : lo64(kU64FastBar);
        v[j] = static_cast<std::uint64_t>(lo64(c.v));
        n64[j] = nn;
        span[j] = hi - nn; // n64 <= v < hi at entry, so span >= 1
        blk[j] = c.blocks;
        live[j] = true;
    }
    // The loop runs until every lane has left its fast lane. A lane that has
    // left is frozen (live[j] == false), so it takes no extra block, while the
    // others continue; with one lane left the loop is the single-chain spin.
    while (nlive != 0) {
        for (std::size_t j = 0; j < W; ++j) {
            if (!live[j]) {
                continue;
            }
            const std::uint32_t packed = T[v[j] & kMask];
            const unsigned f = packed & kFMask;
            const auto cc = static_cast<std::uint64_t>(packed >> 5u);
            v[j] = (v[j] >> kBlockStep) * static_cast<std::uint64_t>(kP[f]) + cc;
            ++blk[j];
            if (v[j] - n64[j] >= span[j]) { // left the lane (retire or promote)
                live[j] = false;
                --nlive;
            }
        }
    }
    for (std::size_t j = 0; j < W; ++j) {
        cs[j]->v = static_cast<u128>(v[j]);
        cs[j]->blocks = blk[j];
    }
}

// Cap on the multi-chain fast-lane spin width. spin_fastlane overlaps up to
// kWmax independent fast-lane chains in one register loop; a larger width
// overlaps more latency-bound madd chains but needs more live registers, and
// past the register file the per-chain state (v/n64/span/blk/live) spills,
// which is slower. It is a constant so W folds into the spin loop (a runtime W
// would itself spill): 8 under clang, whose madd loop stays in registers
// through W=8 (4 registers per chain x 8, about the 31-GPR limit), and 4 under
// GCC, which has fewer usable GPRs and spills at W >= 6. Results are identical
// for any value.
#if defined(__clang__)
constexpr std::size_t kWmax = 8;
#else
constexpr std::size_t kWmax = 4;
#endif

/// Drains a run of `w` (1 <= w <= W) eligible fast-lane chains through the
/// compile-time spin_fastlane<w>. A lone chain (w == 1) has nothing to overlap
/// with, so block_step's single-chain spin takes it. The recursion over the
/// width instantiates only the widths reachable for kWmax.
template <std::size_t W>
static inline void
spin_dispatch(BlockChain* const* g, std::size_t w, const BlockTable16& tb) noexcept {
    if constexpr (W >= 2) {
        if (w == W) {
            std::array<BlockChain*, W> a{};
            for (std::size_t k = 0; k < W; ++k) {
                a[k] = g[k];
            }
            spin_fastlane<W>(a, tb);
            return;
        }
        spin_dispatch<W - 1>(g, w, tb);
    }
    // W < 2: a lone chain is left to block_step; nothing to overlap.
}

/// Overlaps all `ne` eligible fast-lane chains in register groups of at most
/// kWmax (each group is min(remaining, kWmax) wide); a trailing lone chain is
/// left to block_step. Results are bit-identical for any kWmax (see
/// spin_fastlane): only the interleaving of independent chains changes.
template <std::size_t N>
static inline void drain_fastlane(const std::array<std::size_t, N>& elig,
                                  std::size_t ne,
                                  std::array<BlockChain, N>& chain,
                                  const BlockTable16& tb) noexcept {
    std::size_t j = 0;
    while (j < ne) {
        const std::size_t rem = ne - j;
        const std::size_t w = rem < kWmax ? rem : kWmax;
        BlockChain* g[kWmax];
        for (std::size_t k = 0; k < w; ++k) {
            g[k] = &chain[elig[j + k]];
        }
        spin_dispatch<kWmax>(g, w, tb);
        j += (w >= 2 ? w : 1);
    }
}

} // namespace collatz::step3::detail
