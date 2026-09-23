#include "collatz/frontier.hpp"
#include "collatz/progress.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_emit.hpp"
#include "verify_internal.hpp"
#include "verify_survivor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace collatz::verify::detail {

namespace {

// Builds the Step 2 look-ahead filter and records its build time and cache
// state in `res`.
std::unique_ptr<const step2::Filter>
make_filter(const Options& opts, unsigned nthreads, Result& res) {
    const auto t_filter = Clock::now();
    step2::Params sp{opts.N, opts.A, opts.B, opts.num_bitvectors, nthreads, opts.partial_oee};
    // On-disk packed-bitvector / dip cache (--no-bv-cache turns it off).
    sp.use_bv_cache = opts.bv_cache;
    sp.window_chains = opts.window_chains;
    sp.window_templates_D = opts.window_templates_D;
    // On the heap: the filter's precomputed mod-9 masks alone are ~170 KB.
    auto filter_owner = std::make_unique<const step2::Filter>(sp);
    const double filter_seconds = std::chrono::duration<double>(Clock::now() - t_filter).count();
    res.seconds_filter = filter_seconds;
    res.bv_cache_hit = filter_owner->bv_cache_hit();
    return filter_owner;
}

} // namespace

// A > 0: build the Step 2 look-ahead filter once, then apply its per-survivor
// mask: each set bit a is a candidate n = n0 + a*2^M funneled to Step 3.
step1::Stats run_step2(const RunCtx& ctx, Result& res) {
    const Options& opts = ctx.opts;
    std::vector<Acc>& accs = ctx.accs;
    std::vector<TailBatch>& batches = ctx.batches;
    const std::size_t lanes = ctx.lanes;
    const unsigned nthreads = ctx.nthreads;
    const unsigned M = ctx.M;
    const bool block = opts.block_h != 0;

    const std::unique_ptr<const step2::Filter> filter_owner = make_filter(opts, nthreads, res);
    const step2::Filter& filter = *filter_owner;
    const unsigned twoA = 1u << opts.A;

    // Run invariants for the u64 emit fast path.
    EmitCtx emit_ctx;
    emit_ctx.preflimit_lo = lo64(ctx.preflimit);
    emit_ctx.lanes = lanes;
    emit_ctx.twoA = twoA;
    emit_ctx.nodeM = M;
    // Value width, not depth: a block's numbers can pass 2^64 at any N.
    const bool values_fit_u64 = value_bits(opts) <= 64;
    // Block mode: every Step-1 survivor is lifted into the block once, in
    // the sink below, before Step 2 and Step 3 read it (frontier.hpp).
    const frontier::Lift lift =
        block ? frontier::make_lift(opts.block_h, opts.N, opts.A) : frontier::Lift{};
    std::vector<std::vector<step1::Node>> lift_buf(block ? nthreads : 0u);
    const SurvivorProcessor process_survivor{
        filter, emit_ctx, ctx.preflimit, lanes, twoA, M, values_fit_u64};

    const step1::Stats st = step1::enumerate_parallel_buffered(
        M, nthreads, ctx.s1, [&](unsigned tid, const step1::Node* nodes, std::size_t cnt) {
            Acc& acc = accs[tid];
            if (block) {
                // The one place survivors are lifted into the block.
                std::vector<step1::Node>& lifted = lift_buf[tid];
                lifted.assign(nodes, nodes + cnt);
                for (step1::Node& nd : lifted) {
                    frontier::lift_survivor(nd, lift);
                }
                nodes = lifted.data();
            }
            // Tails go into this thread's SoA batch.
            TailBatch& batch = batches[tid];
            // Burst Step-2 window touch: for each group of kBurst
            // survivors, first demand-load every window line of all of them
            // (Filter::touch2, discarded through an empty asm) in one loop of
            // independent loads, so their DRAM/TLB misses overlap, then
            // process the same survivors in the same order. Only discarded
            // in-bounds loads are added: order, batches, flushes and every
            // fold are unchanged.
            {
                constexpr std::size_t kBurst = 16;
                for (std::size_t i0 = 0; i0 < cnt; i0 += kBurst) {
                    const std::size_t i1 = std::min(cnt, i0 + kBurst);
                    std::uint64_t sink = 0;
                    for (std::size_t j = i0; j < i1; ++j) {
                        sink += filter.touch2(nodes[j]);
                    }
                    asm volatile("" : : "r"(sink)); // keep the demand loads live
                    for (std::size_t j = i0; j < i1; ++j) {
                        process_survivor(acc, batch, nodes[j]);
                    }
                }
            }
            progress::survivors_done(cnt); // --progress-file
        });
    // Drain each thread's trailing SoA batch (it spans survivors).
    for (unsigned t = 0; t < nthreads; ++t) {
        flush_tail_batch(accs[t], batches[t], M, lanes);
        // Final drain of each worker's cross-batch deferred queue before
        // its Acc is reduced (the remainder below the kBatchMax drain threshold).
        drain_defer_queue(accs[t], batches[t].dq, lanes);
    }
    res.seconds_step1_2 = res.seconds_filter + st.seconds;
    return st;
}

} // namespace collatz::verify::detail
