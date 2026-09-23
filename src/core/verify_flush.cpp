#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "verify_fold.hpp"
#include "verify_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace collatz::verify::detail {

/// Finish and fold a full (or trailing) SoA batch of table16 candidates over
/// `lanes` concurrent tail lanes. A single running-record threshold is computed
/// for the whole batch: since acc.maxv only grows, that threshold is a valid
/// lower bound for every candidate's exact-max contract, so a batched candidate
/// does at least as much exact-max work as it would unbatched, never less.
/// finish_block16_n advances all candidates (refilling lanes as they retire),
/// then each is folded independently. The fold is order-independent, so the
/// reported Result (coverage, candidate count, pow block digest, exact global
/// max and argmax) is bit-identical to the unbatched path; only the
/// prefix_walks perf diagnostic may differ.
// NOLINTNEXTLINE(readability-function-size): the fold keeps its sums in locals
void flush_tail_batch(Acc& acc, TailBatch& b, unsigned M, std::size_t lanes) {
    if (b.count == 0) {
        return;
    }
    const u128 R = gate_bar(acc);
    const u128 thr = R > 0 ? R - 1 : 0;
    // The drivers write outs[0, count) unless they report b.dq.folded.
    step3::BlockOutcome* const outs = b.outs.data();
    if (b.lo) {
        step3::finish_block16_n_u64(b.n64, b.m64, b.count, lanes, thr, outs, &b.dq);
    } else {
        step3::finish_block16_n(b.n, b.m, b.count, lanes, thr, outs, &b.dq);
    }
    // steps_total and blocks_total (both the sum of work) accumulate in a local
    // and are written back once; candidates is the batch size. Coverage is not
    // folded here: it depends only on n, so it is folded at push time in
    // SurvivorProcessor, which keeps mix64's multiply constants out of this loop.
    // finalize_record_contender takes Acc& but never touches these fold
    // fields, so the locals avoid a per-candidate spill and reload.
    std::uint64_t loc_work = 0;
    // Accumulate the per-candidate result-digest terms raw in a local u128
    // (reduced mod P once at merge). n depends only on the candidate, but blocks
    // and v_end come from the tail here, so the digest is folded in this loop.
    u128 loc_pow_raw = 0;
    // The gate bar R, cached in a local. Only finalize_record_contender changes
    // acc.bar/have_max, and it runs rarely, so the cache saves reloading
    // acc.have_max + acc.bar (a csel pair) per candidate. It is refreshed after
    // each contender call, which keeps the gate exact.
    u128 rrec = gate_bar(acc);
    const std::size_t cnt = b.count;
    // Batch-level record-gate test: pass 1 (overflow resolve + exact-max gate)
    // does nothing unless a candidate overflowed, keeps a live prefix bound, or
    // its tail max can reach the running record. A cheap scan of the tail
    // outputs decides it for the whole batch; otherwise pass 1 is skipped
    // (bit-identical: the skipped work is provably empty).
    bool need_pass1 = b.any_hp;
    // A folded batch did not write outs: read the proved-equal b.m instead.
    const bool no_outs = b.dq.folded;
    // The driver resets scan_needed on every call. When it is false the driver
    // proved the scan below returns false (every outcome came from an admitted
    // fast-lane load: overflow == false, max_seen < hp <= thr >> 10 < R ==
    // rrec; see finish_block16_n_impl), so the scan is skipped.
    const bool proof_skip = !b.dq.scan_needed;
    if (!need_pass1 && !proof_skip) {
        if (no_outs) {
            // A folded batch's outcomes are max_seen == m, overflow == false
            // (see DeferQueue::folded), and outs was not written, so the same
            // OR-reduction runs on b.m.
            unsigned r0 = 0;
            if (b.lo) {
                for (std::size_t i = 0; i < cnt; ++i) {
                    r0 |= static_cast<unsigned>(b.mv(i) >= rrec);
                }
            } else {
                for (std::size_t i = 0; i < cnt; ++i) {
                    r0 |= static_cast<unsigned>(b.m[i] >= rrec);
                }
            }
            need_pass1 = r0 != 0;
        } else {
            // The scan is OR_i(overflow_i || max_seen_i >= rrec) (cnt >= 1
            // here), reduced with four independent OR accumulators: the
            // loop-carried chain becomes a 1-cycle OR and the u128 compares are
            // independent.
            unsigned r0 = 0;
            unsigned r1 = 0;
            unsigned r2 = 0;
            unsigned r3 = 0;
            std::size_t i = 0;
            for (; i + 4 <= cnt; i += 4) {
                r0 |= static_cast<unsigned>(outs[i].overflow) |
                      static_cast<unsigned>(outs[i].max_seen >= rrec);
                r1 |= static_cast<unsigned>(outs[i + 1].overflow) |
                      static_cast<unsigned>(outs[i + 1].max_seen >= rrec);
                r2 |= static_cast<unsigned>(outs[i + 2].overflow) |
                      static_cast<unsigned>(outs[i + 2].max_seen >= rrec);
                r3 |= static_cast<unsigned>(outs[i + 3].overflow) |
                      static_cast<unsigned>(outs[i + 3].max_seen >= rrec);
            }
            for (; i < cnt; ++i) {
                r0 |= static_cast<unsigned>(outs[i].overflow) |
                      static_cast<unsigned>(outs[i].max_seen >= rrec);
            }
            need_pass1 = (r0 | r1 | r2 | r3) != 0;
        }
    }
    const std::size_t p1n = need_pass1 ? cnt : 0;
    // Pass 1: resolve overflowed tails in place and run the record gate (rare calls).
    for (std::size_t i = 0; i < p1n; ++i) {
        std::uint64_t work = 0;
        u128 tail_max = 0;
        u128 v_end = 0;
        bool exceeds = false;
        bool overflowed = false;
        if (no_outs) {
            // Folded-batch outcome is (max_seen = m, overflow = false); blocks /
            // v_end are folded already and unused below when !overflowed.
            tail_max = b.mv(i);
        } else {
            resolve_block_tail(
                b.nv(i), b.mv(i), outs[i], work, tail_max, v_end, exceeds, overflowed);
        }
        if (overflowed) {
            outs[i].blocks = work;
            outs[i].v_end = v_end;
            outs[i].max_seen = tail_max;
        }
        if (exceeds) {
            ++acc.overflow; // walks past 2^128-1 only (see finalize_candidate)
        }
        // Threshold-guarded exact global-max reconstruction (mirrors
        // finalize_candidate; prefix_ub computed lazily, skipped for the
        // has_prefix == false candidates whose prefix is below the record).
        const bool tail_ge = tail_max >= rrec;
        const bool hp = b.hp[i];
        u128 pub = 0;
        if (!tail_ge) {
            if (!hp) {
                continue;
            }
            pub = prefix_ub(b.pmax[i], b.smax[i], b.a[i]);
            if (pub < rrec) {
                continue;
            }
        } else if (hp) {
            pub = prefix_ub(b.pmax[i], b.smax[i], b.a[i]);
        }
        finalize_record_contender(acc, b.nv(i), hp, M, tail_max, tail_ge, pub);
        rrec = gate_bar(acc);
    }
    // Pass 2: call-free digest/work fold, so the u128 accumulator stays in registers.
    if (b.dq.folded) {
        // Fold-inline: the fast-lane spin already summed blocks and the pow-v5
        // term (same result_digest_term, raw u128) for every candidate not
        // queued this batch, and did not write outs[i].blocks / v_end. Pass 1
        // above cannot have changed those either: it rewrites an outcome only if
        // overflowed, and every folded-path outcome has overflow == false
        // (admitted lanes, n <= 2 base cases and queued candidates). Queued
        // candidates are folded at drain, as on the pass-2 path.
        loc_work = b.dq.fw;
        loc_pow_raw = b.dq.fp;
        b.dq.fw = 0;
        b.dq.fp = 0;
    } else {
        for (std::size_t i = 0; i < cnt; ++i) {
            if (b.dq.deferred[i] == b.dq.epoch) {
                continue; // queued for a full-lane drain; work/pow folded then
            }
            loc_work += outs[i].blocks;
            loc_pow_raw += result_digest_term(b.nv(i), outs[i].blocks, outs[i].v_end);
        }
    }
    // Coverage is folded at push time (see SurvivorProcessor), not here.
    acc.candidates += cnt;
    acc.steps_total += loc_work;
    acc.blocks_total += loc_work; // diagnostic block-count sum
    acc.pow_raw += loc_pow_raw;   // result digest (reduced mod P at merge)
    if (b.any_hp) {
        // Restore the hp array to all-false so the next batch's kPrefix==false
        // emission can skip the per-candidate store; only paid when a true was set.
        std::fill_n(b.hp, cnt, false);
        b.any_hp = false;
    }
    // The drain sits outside the pass-1 gate: it runs its own exact-tail record
    // check against the drain-time record, so skipping pass 1 above (which for a
    // deferred entry only sees the lower-bound seed max) never drops a record.
    if (b.dq.size() >= step3::DeferQueue::kBatchMax) {
        drain_defer_queue(acc, b.dq, lanes);
    }
    b.count = 0;
    b.lo = true; // the next fill starts in the u64 columns
}

} // namespace collatz::verify::detail
