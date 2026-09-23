// Step 1 of Angeltveit's sieve: the residue-class DFS and its sieves. The
// mathematical background and the meaning of every field are in
// collatz/step1.hpp.
//
// This file has the enumerators; step1_dfs.cpp has the DFS and step1_sieve.hpp the
// sieves.

#include "collatz/step1.hpp"

#include "collatz/preimage_chain.hpp"
#include "step1_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace collatz::step1 {

using namespace detail;

Stats enumerate(unsigned M, const Options& opts, const std::function<void(const Node&)>& sink) {
    Stats st;
    const auto t0 = std::chrono::steady_clock::now();
    if (M >= root_k) {
        assert(M <= 76 && "M must be <= 76 to keep T^k(n0) < 3^M < 2^128");
        assert(opts.mod3t <= preimage::max_t && "mod3t exceeds supported range");
        std::unique_ptr<preimage::ChainTable> own_chain;
        const preimage::ChainTable* chain = chain_table_for(opts, own_chain);
        Ctx ctx{M, &opts, &sink, 0, nullptr, chain};
        case_params(opts, ctx.case_depth, ctx.case_low);
        set_dfs_width(ctx, opts, M);
        dfs_root(ctx, st);
    }
    const auto t1 = std::chrono::steady_clock::now();
    st.seconds = std::chrono::duration<double>(t1 - t0).count();
    return st;
}

Stats enumerate_parallel(unsigned M,
                         unsigned threads,
                         const Options& opts,
                         const std::function<void(const Node&)>& sink) {
    const unsigned split_depth = kSplitDepth;
    if (threads <= 1 || M <= split_depth || M < root_k) {
        return enumerate(M, opts, sink);
    }

    Stats total;
    const auto t0 = std::chrono::steady_clock::now();

    assert(opts.mod3t <= preimage::max_t && "mod3t exceeds supported range");
    std::unique_ptr<preimage::ChainTable> own_chain;
    const preimage::ChainTable* chain = chain_table_for(opts, own_chain);

    // The serial prelude expands the tree to split_depth and collects the subtree
    // roots.
    std::vector<Frame> tasks;
    {
        assert(M <= 76 && "M must be <= 76 to keep T^k(n0) < 3^M < 2^128");
        Ctx ctx{M, &opts, &sink, split_depth, &tasks, chain};
        case_params(opts, ctx.case_depth, ctx.case_low);
        set_dfs_width(ctx, opts, M);
        dfs_root(ctx, total);
    }

    // Each worker takes tasks by index and accumulates each subtree into a
    // private Stats, so only `sink` and `on_exclude` must be thread-safe.
    const unsigned nthreads = std::min<unsigned>(threads, static_cast<unsigned>(tasks.size()));
    std::vector<Stats> local(nthreads);
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    std::atomic<std::size_t> next_task{0};

    for (unsigned t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t]() {
            Ctx ctx{M, &opts, &sink, 0, nullptr, chain};
            case_params(opts, ctx.case_depth, ctx.case_low);
            set_dfs_width(ctx, opts, M);
            // Stats accumulate on the worker's stack and are published once at the
            // end: adjacent local[] entries (16-byte Stats) share cache lines, so
            // increments into local[t] would false-share.
            Stats mine;
            for (;;) {
                const std::size_t idx = next_task.fetch_add(1);
                if (idx >= tasks.size()) {
                    break;
                }
                const Frame& fr = tasks[idx];
                dfs_task(ctx, mine, fr);
            }
            local[t] = mine;
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    for (const Stats& s : local) {
        merge_stats(total, s);
    }

    const auto t1 = std::chrono::steady_clock::now();
    total.seconds = std::chrono::duration<double>(t1 - t0).count();
    return total;
}

} // namespace collatz::step1
