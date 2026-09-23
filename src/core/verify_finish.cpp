#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_fold.hpp"
#include "verify_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace collatz::verify::detail {

/// Finish and fold one candidate n over its whole trajectory, so its tail
/// maximum bounds its excursion (finalize_candidate). Without `m_overflowed`, m
/// equals n (the preflight) and the 16-step block recurrence walks from it.
/// `m_overflowed` is the Step-2 a-loop escape hatch (m_a = m + 3^f*a beyond
/// u128): the candidate is finished by GMP from n and m is unused. Unreachable
/// for N <= 64.
void process_candidate(Acc& acc, u128 n, u128 m, bool m_overflowed) {
    std::uint64_t work = 0;
    u128 tail_max = 0;
    u128 v_end = 0;
    bool overflowed = false;
    bool exceeds = false;
    const u128 R = gate_bar(acc);
    const u128 thr = R > 0 ? R - 1 : 0;

    if (m_overflowed) {
        std::string dec;
        const step3::BlockOutcome bo = step3::finish_block16_oracle(n, n, &dec);
        work = bo.blocks;
        tail_max = bo.max_seen;
        v_end = bo.v_end;
        exceeds = !dec.empty();
    } else {
        resolve_block_tail(
            n, m, step3::finish_block16(n, m, thr), work, tail_max, v_end, exceeds, overflowed);
    }

    finalize_candidate(acc, n, work, v_end, tail_max, exceeds);
}

/// Drain a thread's cross-batch deferred queue: finish all queued tails over
/// full lanes, then fold their work/pow and offer the record contenders among
/// them. The candidate count was already folded at batch time on the seeded
/// outs. has_prefix is false here: a contender's excursion is walked from n
/// (finalize_record_contender with tail_ge), so its prefix is included. Folding
/// later is bit-identical: every Result fold is order-independent.
void drain_defer_queue(Acc& acc, step3::DeferQueue& q, std::size_t lanes) {
    if (q.size() == 0) {
        return;
    }
    const u128 R = gate_bar(acc);
    const u128 thr = R > 0 ? R - 1 : 0;
    static thread_local std::vector<step3::BlockOutcome> dout;
    dout.resize(q.size());
    step3::drain_deferred(q, lanes, thr, dout.data());
    std::uint64_t loc_work = 0;
    u128 loc_pow_raw = 0;
    u128 rrec = R;
    for (std::size_t k = 0; k < q.size(); ++k) {
        std::uint64_t work = 0;
        u128 tail_max = 0;
        u128 v_end = 0;
        bool exceeds = false;
        bool overflowed = false;
        resolve_block_tail(q.n[k], q.m[k], dout[k], work, tail_max, v_end, exceeds, overflowed);
        if (exceeds) {
            ++acc.overflow; // walks past 2^128-1 only (see finalize_candidate)
        }
        if (tail_max >= rrec) {
            finalize_record_contender(acc, q.n[k], false, /*M=*/0, tail_max, true, 0);
            rrec = gate_bar(acc);
        }
        loc_work += work;
        loc_pow_raw += result_digest_term(q.n[k], work, v_end);
    }
    acc.steps_total += loc_work;
    acc.blocks_total += loc_work;
    acc.pow_raw += loc_pow_raw;
    q.clear();
}

} // namespace collatz::verify::detail
