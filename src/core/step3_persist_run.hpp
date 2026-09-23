#pragma once

// Persistent u64 fast-lane spin, part 2: the per-lane block step, the spin loop and
// the batch driver persist_batch.

#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "step3_persist.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace collatz::step3::detail {

/// One block for lane J. With v, n < hi <= 2^54 the difference v - n has its
/// top 10 bits all set iff v < n and all clear otherwise, so
/// (v - n) >> (63 - J % 8) is 2^(J%8+1) - 1 on descent and 0 otherwise; the
/// XOR of these prefix masks over a group of 8 lanes decodes (x ^ x >> 1) to
/// exactly the set of descending lanes. acc |= v flags v >= hi (hi = 2^hb).
template <std::size_t J, std::size_t W, std::size_t G>
[[gnu::always_inline]] static inline void persist_step(PersistState<W>& s,
                                                       const PersistTab* T,
                                                       std::uint64_t (&xg)[G],
                                                       std::uint64_t& acc) noexcept {
    const BlockMC e = T[s.v[J] & 0xFFFFu];
    s.v[J] = (s.v[J] >> kBlockStep) * e.m + e.c;
    // Prefetches the next block's entry so its L2 hit in the 1 MB table overlaps
    // the exit service (a hint only). The address &T[v & 0xFFFF] is
    // written as the byte offset (v << log2(sizeof e)) & (0xFFFF << log2(sizeof
    // e)), which folds the and + lsl into one ubfiz.
    {
        constexpr unsigned kSh = std::countr_zero(sizeof(PersistTab));
        static_assert((std::size_t{1} << kSh) == sizeof(PersistTab), "table entry size");
        __builtin_prefetch(reinterpret_cast<const char*>(T) +
                               ((s.v[J] << kSh) & (std::uint64_t{0xFFFFu} << kSh)),
                           0,
                           3);
    }
    xg[J / 8] ^= (s.v[J] - s.n64[J]) >> (63u - J % 8u);
    acc |= s.v[J];
}

template <std::size_t W, class C, class X, std::size_t... J>
[[gnu::always_inline]] static inline void
persist_run(const X& x, unsigned hb, std::index_sequence<J...> /*unused*/) noexcept {
    constexpr std::size_t G = (W + 7) / 8;
    PersistState<W> s;
    C c;
    c.nlive = W;
    (persist_fetch(s, c, x, J), ...);
    const PersistTab* const T = x.T;
    while (c.nlive != 0) {
        ++c.it;
        std::uint64_t xg[G]{};
        std::uint64_t acc = 0;
        (persist_step<J>(s, T, xg, acc), ...);
        std::uint64_t any = acc >> hb;
        for (std::size_t g = 0; g < G; ++g) {
            any |= xg[g];
        }
        if (any == 0) [[unlikely]] {
            continue;
        }
        if ((acc >> hb) != 0) [[unlikely]] {
            // Some lane reached v >= hi, so the prefix masks may be wrong and each
            // lane is tested directly.
            for (std::size_t j = 0; j < W; ++j) {
                if (s.v[j] < s.n64[j] || s.v[j] >= x.hi) {
                    persist_retire(s, c, x, j);
                }
            }
        } else {
            // One exit mask for all G groups. Each group's decode
            // xg[g] ^ (xg[g] >> 1) is the XOR of prefix masks 2^(j%8+1) - 1 with
            // j%8 <= 7, so it lies in bits 0..7; shifting group g by 8g packs
            // lane j at bit j, and one ctz loop services the lanes in ascending
            // j, group 0 first, so refills, outs and deferrals follow lane order.
            static_assert(W <= 64, "fused exit mask holds one bit per lane");
            std::uint64_t mk = 0;
            for (std::size_t g = 0; g < G; ++g) {
                mk |= (xg[g] ^ (xg[g] >> 1u)) << (8u * g);
            }
            while (mk != 0) {
                persist_descend(s, c, x, static_cast<std::size_t>(std::countr_zero(mk)));
                mk &= mk - 1;
            }
        }
    }
    if constexpr (C::kFold) {
        x.d->fw += c.fw;
        x.d->fp += c.fp;
    }
}

template <std::size_t W, bool kFold = false, class E = u128>
[[gnu::noinline]] static void persist_fastlane(const E* ns,
                                               const E* ms,
                                               std::size_t count,
                                               std::uint64_t hi,
                                               BlockOutcome* outs,
                                               Deferred& d) noexcept {
    // A power-of-two bar makes v >= hi one OR and shift per iteration; lanes in
    // [bit_floor(hi), hi) are deferred, which is exact.
    const std::uint64_t hp = std::bit_floor(hi);
    const PersistCtx<E> x{ns, ms, count, hp, block_table16_mc().data(), outs, &d};
    using C = std::conditional_t<kFold, PersistCursorFold, PersistCursor>;
    persist_run<W, C>(
        x, static_cast<unsigned>(std::countr_zero(hp)), std::make_index_sequence<W>{});
}

/// The persistent-spin fast path of finish_block16_n_impl, shared by the u128
/// batch and the u64 batch: the caller has bumped the queue epoch and checked
/// lanes >= 2, hi > 3. `inline_drain(d, base)` finishes a chunk's deferred set
/// when no queue is attached (the u128 caller's slot driver; the u64 caller
/// always attaches a queue).
template <class E, class Drain>
static void persist_batch(const E* ns,
                          const E* ms,
                          std::size_t count,
                          std::uint64_t hi,
                          BlockOutcome* outs,
                          DeferQueue* dq,
                          Drain&& inline_drain) noexcept {
    Deferred d;
    // Record-gate proof. The caller passes max_threshold = thr = R - 1 for its
    // running record R (the scan's rrec). Here hi > 3 forces thr >= 2^12, so
    // R = thr + 1 and thr >> 10 < thr < R.
    // persist_fastlane admits a lane only if 3 <= lo64(m) < hp with
    // hp = bit_floor(hi) <= hi <= thr >> 10 (and hi64(m) == 0), and such a
    // lane's outcome is max_seen = m with no overflow. So every admitted
    // candidate, including a mid-walk deferral (which the caller gates on its
    // m = d.maxv), has max_seen < R and no overflow. Only a load-time skip
    // (d.load_skip) or the inline slot-driver drain (dq == nullptr: its exact
    // walk can raise max_seen or overflow) can trip the gate.
    bool sk = false;
    // Fold-inline whenever the queue is attached: every non-queued candidate is
    // retired by descent (persist_descend / persist_retire's descent arm) or is
    // an n <= 2 base case (skip_candidate, which folds its term straight into
    // d, never queued), and each of those is folded into d.fw / d.fp exactly
    // once; queued ones are folded at drain. Without the queue the inline drain
    // below writes outs, so pass 2 must run and the plain spin is used.
    const bool fold = dq != nullptr;
    // d.fw / d.fp are zeroed once per call and accumulate across the
    // Deferred::kCap chunks below, where only d.count and load_skip reset per
    // chunk. The totals are published to dq after the loop.
    d.fw = 0;
    d.fp = 0;
    for (std::size_t base = 0; base < count; base += Deferred::kCap) {
        const std::size_t c = std::min(Deferred::kCap, count - base);
        d.count = 0;
        d.load_skip = false;
        if (fold) {
            persist_fastlane<kPersistW, true>(ns + base, ms + base, c, hi, outs + base, d);
        } else {
            persist_fastlane<kPersistW>(ns + base, ms + base, c, hi, outs + base, d);
        }
        sk |= d.load_skip;
        if (d.count == 0) {
            continue;
        }
        if (dq != nullptr) {
            // The queue accumulates across batches; the caller drains it in
            // full-lane groups and folds these candidates then. The caller's
            // per-batch record gate reads their (lower-bound) landing m; work,
            // pow and the exact tail max are folded at the drain.
            for (std::size_t k = 0; k < d.count; ++k) {
                dq->n.push_back(d.n[k]);
                dq->m.push_back(d.maxv[k]);
                dq->v.push_back(d.v[k]);
                dq->blk.push_back(d.blk[k]);
                dq->deferred[base + d.idx[k]] = dq->epoch;
            }
            continue;
        }
        inline_drain(d, base);
        sk = true; // inline drain outcomes are exact-walk, unbounded
    }
    if (dq != nullptr) {
        dq->scan_needed = sk;
        dq->folded = true;
        dq->fw = d.fw;
        dq->fp = d.fp;
    }
}

} // namespace collatz::step3::detail
