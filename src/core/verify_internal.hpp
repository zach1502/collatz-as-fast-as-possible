#ifndef COLLATZ_CORE_VERIFY_INTERNAL_HPP
#define COLLATZ_CORE_VERIFY_INTERNAL_HPP

// Private declarations shared by the src/core/verify*.cpp pieces of the CPU
// verifier: the per-thread accumulator Acc, the SoA tail batch, the run
// context and the functions that the pieces call across files.

#include "collatz/digest_term.hpp"
#include "collatz/step1.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace collatz::verify::detail {

using Clock = std::chrono::steady_clock;

/// Mersenne prime 2^61 - 1: the field the coverage set-fold lives in, so the
/// fold is associative/commutative and thus thread-count independent.
inline constexpr std::uint64_t kCoveragePrime = 2305843009213693951ULL;

/// All n < 2^kPreflightBits are verified directly in the preflight; this also
/// covers the n >= 99781 precondition of the descent theorems (2^17 = 131072).
inline constexpr unsigned kPreflightBits = 17;

// mix64, rotl64 and the pow-v5 result_digest_term live in
// collatz/digest_term.hpp so the inline fold of step3_persist.hpp calls the same
// function as the pass-2 fold of flush_tail_batch.
using digest::mix64;
using digest::result_digest_term;
using digest::rotl64;

/// Record-threshold state shared read-only by a run's accumulators
/// (Options::record_threshold).
struct RecordThreshold {
    bool fits_u128 = false; ///< x holds the threshold; false: it exceeds 2^128-1.
    u128 x = 0;
    std::string dec;               ///< Canonical decimal of the threshold.
    std::size_t cap = kRecordsCap; ///< Options::records_cap (>= 1).
};

/// Per-thread accumulator. Folds are commutative/associative so the post-join
/// merge is independent of how survivors were distributed across threads.
///
/// (maxv, maxn, have_max) is the thread's running path record R: the largest
/// excursion (step3::first_drop_peak) seen so far and the smallest n attaining
/// it. `bar` gates the exact-max reconstruction (see finalize_candidate): a
/// candidate whose excursion cannot reach the bar is folded without any exact
/// max work.
///
/// `alignas(128)` gives every worker's Acc its own pair of cache lines, so the
/// per-candidate `candidates`/`coverage`/`pow` writes from adjacent threads
/// never false-share a line.
struct alignas(128) Acc {
    std::uint64_t candidates = 0;
    std::uint64_t steps_total = 0;
    u128 coverage_raw = 0; ///< running raw sum of mix64 keys (reduced mod P once at merge)
    /// Result digest (pow): running raw u128 sum of the per-candidate
    /// result-digest terms (reduced mod 2^61-1 once at merge, like coverage_raw).
    u128 pow_raw = 0;
    /// Wrapping u64 sum of the 16-step block counts (Result::blocks_total).
    std::uint64_t blocks_total = 0;
    u128 maxv = 0;
    u128 maxn = 0;
    bool have_max = false;
    bool max_exceeds = false;
    std::uint64_t overflow = 0;
    std::uint64_t masked_out = 0;   ///< Step 2 extensions pruned (A > 0 only).
    std::uint64_t prefix_walks = 0; ///< Candidates that needed an exact prefix re-walk.
    /// Bar the record gates compare against: min(maxv, bar_cap), 0 before the
    /// first record. Equal to maxv unless a record threshold X is set, where
    /// bar_cap = X + 1 keeps every candidate above X exact. A lower bar only
    /// adds exact-max work, never changes the record (finalize_record_contender).
    u128 bar = 0;
    u128 bar_cap = u128_max;
    const RecordThreshold* rt = nullptr; ///< null: no record list.
    /// Hits above rt, unsorted: (n, peak) pairs for peaks below 2^128 and
    /// decimal peaks above 2^128-1 (offer_wide). Bounded by prune_records: only
    /// the rt->cap smallest n of the whole run can reach Result::records.
    std::vector<std::pair<u128, u128>> rec_hits;
    std::vector<RecordHit> rec_wide;
    /// After a prune this thread alone holds rt->cap hits with n <= rec_limit,
    /// so a later hit with a larger n cannot reach Result::records and is
    /// dropped on arrival (rec_dropped records that the cap was hit).
    u128 rec_limit = u128_max;
    bool rec_dropped = false;
    /// Exact record among excursions above 2^128-1 (decimal; empty when none),
    /// offered by the record contenders (offer_wide). It dominates the
    /// saturated maxv.
    std::string wide_v;
    u128 wide_n = 0;
};

/// The record gates' bar (see Acc::bar).
[[nodiscard]] inline u128 gate_bar(const Acc& acc) noexcept {
    return acc.have_max ? acc.bar : static_cast<u128>(0);
}

// Defined in verify_records.cpp.
[[gnu::noinline]] void offer_wide(Acc& acc, u128 n, const std::string& dec);
[[gnu::noinline]] void finalize_record_contender(
    Acc& acc, u128 n, bool has_prefix, unsigned M, u128 tail_max, bool tail_ge, u128 pub);

/// Lanes of the batched tail: how many candidates' dependent chains
/// finish_block16_n interleaves per thread. Results do not depend on it.
inline constexpr std::size_t kTailLanes = 16;

/// Per-thread SoA buffer capacity. The buffer is drained by a refilling
/// `lanes`-wide stepper, so it is sized well above the lane count to keep the
/// refill supplied and amortize the flush/fold bookkeeping.
inline constexpr std::size_t kTailBatchCap = step3::kBatchCap;
static_assert(kTailBatchCap <= step3::DeferQueue::kBatchMax,
              "a tail batch must fit the DeferQueue per-batch mask");

/// Per-thread structure-of-arrays batch of table16 candidates awaiting a
/// lane-parallel tail. Fixed-capacity SoA columns; `count` entries are
/// pending. Every batched candidate has a Step-1 prefix (has_prefix == true)
/// and shares the run's constant M, so only (n, m, pmax, smax, a) vary.
struct TailBatch {
    /// While `lo` is true the pending entries' n and m live in n64 / m64 (every
    /// one fits u64) and n / m are stale. emit_batched_u64 fills them with one
    /// 8-B store each instead of two zero-padded 16-B ones, and the tail
    /// (finish_block16_n_u64) reads them without hi words. A push whose n or m
    /// needs a high word widens the batch (lo = false) for the rest of its fill;
    /// each flush resets lo. The pad keeps m64 off n64 + 4 KiB (cap 512 x 8 B),
    /// a 4K-alias pair.
    alignas(64) std::uint64_t n64[kTailBatchCap];
    std::byte pad64_[64];
    std::uint64_t m64[kTailBatchCap];
    bool lo = true;
    u128 n[kTailBatchCap];
    u128 m[kTailBatchCap];
    u128 pmax[kTailBatchCap];
    u128 smax[kTailBatchCap];
    unsigned a[kTailBatchCap];
    /// Whether this candidate's prefix bound can still reach the running
    /// record. False when the survivor's whole-prefix upper bound was already
    /// below the record at push time (records only grow), so finalize skips the
    /// pmax/smax loads and the u128 prefix_ub multiply. pmax/smax/a are written
    /// only for has_prefix == true entries; the rest carry stale values that
    /// finalize never reads.
    bool hp[kTailBatchCap]{};
    /// OR of hp over the pending entries, maintained at push. The record gate
    /// reads it instead of scanning hp, and the hp array stays all-false for
    /// unwritten/false entries so the common kPrefix==false emission can skip
    /// the per-candidate hp store; a batch that set any true clears hp on flush.
    bool any_hp = false;
    std::size_t count = 0;
    /// Tail outcomes; persistent so a flush does not re-zero 16 KB per batch.
    std::array<step3::BlockOutcome, kTailBatchCap> outs{};
    /// Cross-batch deferred (u128 / above-bar) tail candidates: accumulated over
    /// batches and drained in full-lane groups (see drain_defer_queue).
    step3::DeferQueue dq;

    /// Move the pending u64 entries into the u128 columns (cold: a push whose
    /// n or m needs a high word).
    void widen() noexcept {
        if (lo) {
            for (std::size_t i = 0; i < count; ++i) {
                n[i] = n64[i];
                m[i] = m64[i];
            }
            lo = false;
        }
    }
    [[nodiscard]] u128 nv(std::size_t i) const noexcept {
        return lo ? static_cast<u128>(n64[i]) : n[i];
    }
    [[nodiscard]] u128 mv(std::size_t i) const noexcept {
        return lo ? static_cast<u128>(m64[i]) : m[i];
    }
};
static_assert((kTailBatchCap * sizeof(std::uint64_t) + 64) % 4096 != 0,
              "n64 / m64 must not sit a multiple of 4 KiB apart");

// Defined in verify_finish.cpp.
void process_candidate(Acc& acc, u128 n, u128 m, bool m_overflowed = false);
void drain_defer_queue(Acc& acc, step3::DeferQueue& q, std::size_t lanes);

// Defined in verify_flush.cpp.
void flush_tail_batch(Acc& acc, TailBatch& b, unsigned M, std::size_t lanes);

/// run()'s loop invariants and per-thread state, shared by the A == 0 and
/// A > 0 enumeration paths. Every tail goes through the thread's TailBatch.
struct RunCtx {
    const Options& opts;
    const step1::Options& s1;
    std::vector<Acc>& accs;
    std::vector<TailBatch>& batches;
    u128 preflimit;
    std::size_t lanes;
    unsigned nthreads;
    unsigned M;
};

// Defined in verify_step1.cpp.
u128 run_preflight(
    const Options& opts, bool block, unsigned nthreads, std::vector<Acc>& accs, Result& res);
step1::Stats run_step1_only(const RunCtx& ctx);

// Defined in verify_step2.cpp.
step1::Stats run_step2(const RunCtx& ctx, Result& res);

} // namespace collatz::verify::detail

#endif // COLLATZ_CORE_VERIFY_INTERNAL_HPP
