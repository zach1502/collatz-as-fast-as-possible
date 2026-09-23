#ifndef COLLATZ_CORE_VERIFY_FOLD_HPP
#define COLLATZ_CORE_VERIFY_FOLD_HPP

// Helpers that resolve a candidate's tail and fold it into its thread's Acc.
// They are inline in this header so that the batch flush, the deferred-queue
// drain, the single-candidate path and the per-survivor emission can inline
// them.

#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_internal.hpp"

#include <cstdint>
#include <string>

namespace collatz::verify::detail {

/// Cheap upper bound pmax + smax * a on a candidate's exact prefix maximum,
/// from the per-survivor bounds Step 1 carries incrementally down its DFS (see
/// step1::Node): pmax >= max_j T^j(n0) and smax = max_j 3^{f_j} 2^{M-j}, so
/// max_j (T^j(n0) + 3^{f_j} 2^{M-j} a) <= pmax + smax*a. Saturates to u128_max
/// on overflow (unreachable for N <= 64), which keeps it a valid upper bound
/// and forces the exact walk.
[[nodiscard]] inline u128 prefix_ub(u128 pmax, u128 smax, unsigned a) noexcept {
    bool ovf = false;
    const u128 term = checked_mul(smax, static_cast<u128>(a), ovf);
    if (ovf) {
        return u128_max;
    }
    const u128 ub = checked_add(pmax, term, ovf);
    return ovf ? u128_max : ub;
}

/// Fold one candidate n whose whole trajectory from n has been resolved into
/// (work, tail_max, exceeds). `work` is the 16-step block count to the first
/// block boundary below n.
///
/// pow folds the per-candidate result-digest term and blocks_total sums the
/// block counts (Angeltveit's "numcases"). Neither needs an exact step count
/// or a per-candidate maximum, which lets the table16 tail jump whole blocks
/// and stop at block granularity below n.
///
/// step3_steps_total is the non-wrapping sum of `work`.
///
/// Global max (path record): each thread carries a running record
/// R = (acc.maxv, acc.maxn). The walk starts at n and tail_max covers every
/// value of its blocks, so tail_max bounds the excursion from above: a
/// candidate is a record contender only when tail_max >= R. Otherwise its
/// excursion is provably < R and it is folded with no max work. A contender's
/// excursion is recomputed by finalize_record_contender, because tail_max can
/// include values after the orbit first dropped below n. The record and its
/// smallest-n argmax stay exact.
inline void
finalize_candidate(Acc& acc, u128 n, std::uint64_t work, u128 v_end, u128 tail_max, bool exceeds) {
    ++acc.candidates;
    acc.steps_total += work;
    const std::uint64_t key = lo64(n) ^ rotl64(hi64(n), 32);
    acc.coverage_raw += mix64(key); // reduced mod P once per thread at merge
    // Fold the per-candidate result-digest term (raw in u128, reduced mod P
    // once at merge); blocks_total keeps the plain block-count sum.
    acc.blocks_total += work;
    acc.pow_raw += result_digest_term(n, work, v_end);
    // A block tail counts as an overflow fallback only when its walk passed
    // 2^128-1: a GMP replay that stayed below (an odd w >= (2^128-1)/3 in a
    // block whose exact peak was walked) happens or not depending on the
    // running record bar, so counting it would make overflow_fallbacks depend
    // on the schedule.
    if (exceeds) {
        ++acc.overflow;
    }

    // Threshold-guarded exact global-max reconstruction.
    const u128 R = gate_bar(acc);
    if (tail_max < R) {
        return; // excursion provably below the record: no exact work needed
    }
    // Rare: this candidate could beat or tie the record, so reconstruct it
    // exactly in the out-of-line cold helper (keeps this path canary-free and
    // inlinable).
    finalize_record_contender(acc,
                              n,
                              /*has_prefix=*/false,
                              /*M=*/0,
                              tail_max,
                              /*tail_ge=*/true,
                              /*pub=*/0);
}

/// Resolve a block (table16) tail BlockOutcome into the work/max/flags tuple,
/// falling back to the GMP block replay on overflow. `work` is the block count.
/// `exceeds` says the replayed walk passed 2^128-1; tail_max is then u128_max,
/// which makes the candidate a record contender, and finalize_record_contender
/// offers its excursion to the wide record when that passes 2^128-1 too.
inline void resolve_block_tail(u128 n,
                               u128 m,
                               const step3::BlockOutcome& tail,
                               std::uint64_t& work,
                               u128& tail_max,
                               u128& v_end,
                               bool& exceeds,
                               bool& overflowed) {
    if (tail.overflow) {
        std::string dec;
        const step3::BlockOutcome ko = step3::finish_block16_oracle(n, m, &dec);
        work = ko.blocks;
        tail_max = ko.max_seen;
        v_end = ko.v_end;
        exceeds = !dec.empty();
        overflowed = true;
    } else {
        work = tail.blocks;
        tail_max = tail.max_seen;
        v_end = tail.v_end;
        exceeds = false;
        overflowed = false;
    }
}

} // namespace collatz::verify::detail

#endif // COLLATZ_CORE_VERIFY_FOLD_HPP
