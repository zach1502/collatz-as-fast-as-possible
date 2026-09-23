#ifndef COLLATZ_STEP3_BLOCK_HPP
#define COLLATZ_STEP3_BLOCK_HPP

// Block-granular Step 3 finishers and their batch drivers (collatz/step3.hpp
// includes this file).
//
// Block-granular 16-step table finisher. The block kernel (finish_block16)
// drives the 16-step table recurrence without single-stepping the final block.
// Starting from m = T^M(n), while v >= n it maps r = v mod 2^16 through a packed
// table TBL16[r] = (T^16(r) << 5) | f (f = odd-step count of r's 16-step
// block), then jumps v = (v >> 16) * 3^f + T^16(r) and counts one block. It
// stops the first time a block lands below n: the value below n is already
// covered by the induction, and a later rise above n on the same trajectory is
// irrelevant to the descent certificate. So it reports the number of 16-step
// blocks (not exact T-steps) until the trajectory first drops below n. This is
// the count Angeltveit's verifier accumulates (his "numcases"); it needs no
// exact step count and no per-candidate exact maximum, so the final block is
// never single-stepped.

#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace collatz::step3 {

/// Tail-batch capacity, shared by verify_internal.hpp's kTailBatchCap, DeferQueue's
/// per-batch stamp mask (kBatchMax) and step3_persist.hpp's Deferred::kCap so the three
/// always agree. The value only reshapes batching: every Result fold is
/// order/chunking-independent.
inline constexpr std::size_t kBatchCap = 512;
static_assert(kBatchCap >= 64, "a tail batch must hold at least one mask word");

/// Result of the block-granular finisher.
struct BlockOutcome {
    /// Number of 16-step blocks executed until the trajectory first drops below
    /// n. Identical whether computed in u128 or replayed in GMP.
    std::uint64_t blocks = 0;
    /// Result-digest input: the landing value after the last block, i.e.
    /// the first block-boundary value strictly below n (for the n <= 2 base case
    /// it is n, with blocks == 0). Together with (n, blocks) it is folded into
    /// the strong order-independent result digest. Identical whether computed in
    /// u128 or replayed in GMP; on overflow it is meaningless (the caller replays
    /// via the oracle, which reproduces the same v_end). Unreachable overflow for
    /// N <= 64.
    u128 v_end = 0;
    /// Maximum value seen on the walked blocks, every value of every block from
    /// m to the landing below n. The walk tests "< n" only at block ends, so this
    /// can include values after n's orbit first dropped below n: it bounds the
    /// excursion past m (first_drop_peak) from above and can exceed it. Exact
    /// whenever the true maximum exceeds `max_threshold`; otherwise a lower bound
    /// not exceeding `max_threshold` (so `max_threshold == 0` makes it exact).
    /// The block that contains a candidate's peak is single-stepped only when
    /// its rigorous upper bound could reach the caller's bar, so the common
    /// (below-bar) candidate does zero single-stepping.
    u128 max_seen = 0;
    /// True iff the u128 block multiply (v >> 16) * 3^f + c would overflow (or a
    /// contender block's exact single-step overflowed); `blocks`/`max_seen` are
    /// then meaningless and the caller must retry via finish_block16_oracle,
    /// which reproduces the same block count. Unreachable for N <= 64.
    bool overflow = false;
};

/// Block-granular 16-step table finisher. Reports the number of
/// 16-step blocks to first descent below n and, subject to `max_threshold`, the
/// exact maximum excursion (see BlockOutcome). Sets `overflow` and bails if the
/// block multiply would exceed u128 (caller falls back to finish_block16_oracle).
[[nodiscard]] BlockOutcome finish_block16(u128 n, u128 m, u128 max_threshold = 0) noexcept;

/// Per-thread cross-batch deferred-candidate queue. The persistent fast-lane
/// spin hands u128 / above-bar tail candidates to the block-granular slot
/// driver. Instead of draining them once per kBatchMax-candidate batch, in small
/// calls with few lanes busy, finish_block16_n appends them here so the caller
/// can drain many at once over full lanes. Each entry carries the state the
/// slot driver resumes from: the original n, the original
/// m = T^M(n) (max_seen seed + overflow fallback), the block-aligned hand-off
/// value v, and the block count executed so far. `deferred[i]` flags which of
/// the current batch's [0, count) candidates were queued, so the caller folds
/// their work/pow at drain rather than in the batch. Draining later is
/// bit-identical: all Result folds are order-independent (PipelineInvariants).
struct DeferQueue {
    /// max candidates per driver call.
    static constexpr std::size_t kBatchMax = kBatchCap;
    std::vector<u128> n;            ///< original candidate n.
    std::vector<u128> m;            ///< original m = T^M(n).
    std::vector<u128> v;            ///< block-aligned resume value at hand-off.
    std::vector<std::uint64_t> blk; ///< blocks already executed at hand-off.
    /// Per-batch "deferred" mark as an epoch stamp (not a bool), so a batch marks
    /// its queued indices without a kBatchMax-entry clear: `epoch` is bumped once per
    /// driver call and index i is queued this batch iff deferred[i] == epoch.
    std::uint64_t deferred[kBatchMax] = {};
    std::uint64_t epoch = 0;
    /// Read by the caller's batch record-gate skip: false iff
    /// the last driver call took the u64 fast lane for the whole batch with no
    /// load-time skip, which proves every outcome has overflow == false and
    /// max_seen < the batch threshold's record (see finish_block16_n_impl), so
    /// the caller's record-gate scan would return false. Reset to true at every
    /// call; every other path leaves it true (the scan runs).
    bool scan_needed = true;
    /// Fold-inline: true iff the last driver call took the u64 fast lane
    /// for the whole batch with this queue attached, so every candidate that is
    /// not queued this batch (deferred[i] != epoch) had its block count and
    /// pow-v5 term summed into fw / fp by the spin (exactly once, with the same
    /// result_digest_term as pass 2), and the caller does not read outs: every
    /// outcome of the batch, queued or not, is max_seen == m, overflow == false
    /// for the record gate, so the caller reads m instead. The caller then
    /// takes fw / fp instead of running pass 2 and zeroes them. Reset to false
    /// at every call.
    bool folded = false;
    std::uint64_t fw = 0; ///< fold-inline: sum of blocks over folded candidates.
    u128 fp = 0;          ///< fold-inline: raw u128 sum of pow-v5 terms (reduced mod P at merge).
    /// Length stays below 2 * kBatchMax, so push_back in the noexcept driver
    /// never reallocates.
    DeferQueue() {
        n.reserve(2 * kBatchMax);
        m.reserve(2 * kBatchMax);
        v.reserve(2 * kBatchMax);
        blk.reserve(2 * kBatchMax);
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return n.size();
    }
    void clear() noexcept {
        n.clear();
        m.clear();
        v.clear();
        blk.clear();
    }
};

/// Structure-of-arrays, refilling L-lane block driver:
/// finish `count` independent block-granular tails while keeping only
/// `lanes` (<= 32) dependent chains in flight at once. `n[i]`/`m[i]` are the
/// i-th candidate's start pair (m[i] = T^k(n[i]) >= n[i]); `out[i]` receives its
/// BlockOutcome. All candidates share one `max_threshold`. The driver seeds
/// `lanes` slots, advances every live slot by one 16-step block per round, and
/// the instant a slot's trajectory drops below its n it retires and is refilled
/// with the next pending candidate (swap-in, so no slot idles while work
/// remains), turning the per-candidate latency-bound dependent chain into
/// cross-lane throughput on an out-of-order core. Each `out[i]` is bit-for-bit
/// identical to `finish_block16(n[i], m[i], max_threshold)` (same `blocks`,
/// `max_seen`, `overflow`); the lane count only reorders overlap, never results.
/// `n`, `m` and `out` must point to at least `count` elements. `lanes` is
/// clamped to [1, 32] and to `count`.
///
/// `dq`: when non-null, deferred candidates are appended to it instead of
/// being drained inline, and the caller drains the queue in large groups; their
/// block count, digest and exact tail max are folded at drain. A batch the fast
/// lane takes then reports its outcomes through DeferQueue::folded instead of
/// `out`. With a queue, `count` must not exceed DeferQueue::kBatchMax. When
/// null the deferred set is drained inline.
void finish_block16_n(const u128* n,
                      const u128* m,
                      std::size_t count,
                      std::size_t lanes,
                      u128 max_threshold,
                      BlockOutcome* out,
                      DeferQueue* dq = nullptr) noexcept;

/// finish_block16_n for a batch whose every n and m fits u64, passed as u64
/// arrays. Outcomes, `dq` hand-off and the fold-inline sums are identical to
/// finish_block16_n on the zero-extended arrays; the fast lane skips the
/// hi-word loads and test. A call the fast lane cannot take (no queue,
/// lanes < 2 or hi <= 3) zero-extends into finish_block16_n. With a queue,
/// `count` must not exceed DeferQueue::kBatchMax.
void finish_block16_n_u64(const std::uint64_t* n,
                          const std::uint64_t* m,
                          std::size_t count,
                          std::size_t lanes,
                          u128 max_threshold,
                          BlockOutcome* out,
                          DeferQueue* dq) noexcept;

/// Drains a DeferQueue: runs the block-granular slot driver over all queued
/// candidates at once (full lanes), resuming each from its hand-off value with
/// its own block count. `out` receives `q.size()` BlockOutcomes, each bit-for-
/// bit identical to what a per-batch inline drain would have produced (the lane
/// count and grouping only reorder overlap, never results). The caller folds
/// them and clears the queue.
void drain_deferred(const DeferQueue& q,
                    std::size_t lanes,
                    u128 max_threshold,
                    BlockOutcome* out) noexcept;

/// GMP replay of the block recurrence: exact and overflow-free, producing the
/// same block count as finish_block16 would (same TBL16 landing arithmetic, big
/// integers) and the exact `max_seen` of the walked blocks (every block
/// single-stepped, as defined at BlockOutcome::max_seen). Used as the fallback
/// when finish_block16 overflows u128. `max_dec` as in finish_oracle, for that
/// block maximum. Unreachable for N <= 64.
[[nodiscard]] BlockOutcome finish_block16_oracle(u128 n, u128 m, std::string* max_dec = nullptr);

} // namespace collatz::step3

#endif // COLLATZ_STEP3_BLOCK_HPP
