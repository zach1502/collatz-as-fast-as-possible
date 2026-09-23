#include "collatz/progress.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_emit.hpp"
#include "verify_fold.hpp"
#include "verify_internal.hpp"

#include <cstddef>
#include <vector>

namespace collatz::verify::detail {

// Preflight: verify every n < min(2^17, 2^N) directly. Serial into the
// first accumulator; the counts are cheap and this fixes the induction base.
// Block mode has no preflight: its induction base is the hypothesis that
// every n < block_start converges, and validate_block keeps the block
// above 2^17, the floor the descent theorems need (preflight_* stay 0).
// Returns the preflight limit.
u128 run_preflight(
    const Options& opts, bool block, unsigned nthreads, std::vector<Acc>& accs, Result& res) {
    const unsigned preflight_bits =
        block ? 0u : (opts.N < kPreflightBits ? opts.N : kPreflightBits);
    const u128 preflimit = block ? static_cast<u128>(0) : static_cast<u128>(1) << preflight_bits;
    for (u128 n = 1; n < preflimit; ++n) {
        process_candidate(accs[0], n, n);
    }

    // Snapshot the preflight's contribution to the mergeable scalars, so a merge
    // of case slices (step1::Options::case_c of case_k) can subtract the
    // (k-1)-fold duplicate (the preflight above is re-run identically in every
    // slice; it is not sliced).
    // coverage/pow are reduced mod P here to match the final merge reduction; the
    // preflight folds only into accs[0], and nothing has run since, so accs[0]
    // holds exactly the preflight partials here.
    res.preflight_candidates = accs[0].candidates;
    res.preflight_coverage = mod_mersenne61_u128(accs[0].coverage_raw) % kCoveragePrime;
    res.preflight_pow = mod_mersenne61_u128(accs[0].pow_raw) % kCoveragePrime;
    res.preflight_blocks = accs[0].blocks_total;
    res.preflight_step3_steps = accs[0].steps_total;

    // Seed every worker's running record R with the preflight maximum so worker
    // threads start with a meaningful bar (fewer early exact reconstructions).
    // This changes only how much exact-max work each thread does, never the
    // reported result: the final (maxv, maxn) is the exact global max with the
    // smallest-n tie-break regardless of the starting bar.
    for (unsigned t = 1; t < nthreads; ++t) {
        accs[t].maxv = accs[0].maxv;
        accs[t].maxn = accs[0].maxn;
        accs[t].have_max = accs[0].have_max;
        accs[t].bar = accs[0].bar;
    }
    return preflimit;
}

// A == 0: each Step-1 survivor is a single candidate, finished by Step 3
// through the thread's SoA tail batch.
step1::Stats run_step1_only(const RunCtx& ctx) {
    std::vector<Acc>& accs = ctx.accs;
    std::vector<TailBatch>& batches = ctx.batches;
    const u128 preflimit = ctx.preflimit;
    const std::size_t lanes = ctx.lanes;
    const unsigned nthreads = ctx.nthreads;
    const unsigned M = ctx.M;
    const step1::Stats st = step1::enumerate_parallel_buffered(
        M, nthreads, ctx.s1, [&](unsigned tid, const step1::Node* nodes, std::size_t cnt) {
            Acc& acc = accs[tid];
            for (std::size_t i = 0; i < cnt; ++i) {
                const step1::Node& node = nodes[i];
                if (node.n0 < preflimit) {
                    continue; // already discharged by the preflight
                }
                // a=0 here, so the survivor's whole-prefix bound is
                // just prefix_ub(pmax, smax, 0) = pmax. If it is already
                // below the running record it can never contend (records
                // only grow), so mark has_prefix false and skip the
                // pmax/smax stores + prefix_ub multiply downstream.
                const bool prefix_rel =
                    !acc.have_max || prefix_ub(node.pmax, node.smax, 0) >= acc.bar;
                // Fold coverage at push; the flush does not.
                // Key = lo64(n0) ^ rotl64(hi64(n0),32); hi64(n0)==0 for
                // N<=64 (a==0), and carries the high limb for N>64.
                acc.coverage_raw += mix64(lo64(node.n0) ^ rotl64(hi64(node.n0), 32));
                push_tail_batch(acc,
                                batches[tid],
                                M,
                                lanes,
                                node.n0,
                                node.m,
                                node.pmax,
                                node.smax,
                                /*a=*/0,
                                prefix_rel);
            }
            progress::survivors_done(cnt); // --progress-file
        });
    for (unsigned t = 0; t < nthreads; ++t) {
        flush_tail_batch(accs[t], batches[t], M, lanes);
        drain_defer_queue(accs[t], batches[t].dq, lanes);
    }
    return st;
}

} // namespace collatz::verify::detail
