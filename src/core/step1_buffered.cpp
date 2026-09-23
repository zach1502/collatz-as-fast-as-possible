// Step 1 buffered parallel enumerator (enumerate_parallel_buffered): each worker
// fills a private survivor buffer and hands full batches to the caller's sink.

#include "collatz/preimage_chain.hpp"
#include "collatz/progress.hpp"
#include "collatz/step1.hpp"
#include "step1_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace collatz::step1 {

using namespace detail;

namespace {

/// Hands the survivors left in `buf` at the end of a worker's run to `sink`.
void flush_rest(const BatchSink& sink, unsigned tid, EmitBuf& buf) {
    if (buf.n != 0) {
        progress::survivors_found(buf.n);
        sink(tid, buf.v.data(), buf.n);
        buf.n = 0;
    }
}

} // namespace

Stats enumerate_parallel_buffered(unsigned M,
                                  unsigned threads,
                                  const Options& opts,
                                  const BatchSink& consume_batch,
                                  const WorkerDoneHook& on_worker_done) {
    const std::size_t cap = 4096; // survivors per BatchSink call
    const unsigned split_depth = kSplitDepth;

    Stats total;
    const auto t0 = std::chrono::steady_clock::now();

    assert(opts.mod3t <= preimage::max_t && "mod3t exceeds supported range");
    std::unique_ptr<preimage::ChainTable> own_chain;
    const preimage::ChainTable* chain = chain_table_for(opts, own_chain);

    // Serial path (or a tree too shallow to split): one tid-0 buffer.
    if (threads <= 1 || M <= split_depth || M < root_k) {
        // --progress-file: the whole tree is one task.
        const std::uint64_t one_task = 1;
        progress::start_tasks({&one_task, 1});
        if (M >= root_k) {
            assert(M <= 76 && "M must be <= 76 to keep T^k(n0) < 3^M < 2^128");
            EmitBuf buf(cap);
            Ctx ctx{M, &opts, nullptr, 0, nullptr, chain, &buf, &consume_batch, 0u, cap};
            case_params(opts, ctx.case_depth, ctx.case_low);
            set_dfs_width(ctx, opts, M);
            dfs_root(ctx, total);
            flush_rest(consume_batch, 0u, buf);
            if (on_worker_done) {
                on_worker_done(0u);
            }
        }
        progress::task_finished(0, total.survivors);
        total.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return total;
    }

    // The serial prelude expands the tree to split_depth and collects the subtree
    // roots. Survivors appear only at depth M > split_depth, so the prelude emits
    // none; it still gets a tid-0 buffer, flushed after it.
    std::vector<Frame> tasks;
    EmitBuf prelude_buf(cap);
    {
        assert(M <= 76 && "M must be <= 76 to keep T^k(n0) < 3^M < 2^128");
        Ctx ctx{
            M, &opts, nullptr, split_depth, &tasks, chain, &prelude_buf, &consume_batch, 0u, cap};
        case_params(opts, ctx.case_depth, ctx.case_low);
        set_dfs_width(ctx, opts, M);
        dfs_root(ctx, total);
    }
    flush_rest(consume_batch, 0u, prelude_buf);
    {
        // --progress-file: tasks are counted by the odd steps of their root,
        // which the survivor estimate weighs them by.
        std::array<std::uint64_t, progress::kTaskClasses> per_class{};
        for (const Frame& fr : tasks) {
            ++per_class[progress::task_class(fr.f)];
        }
        progress::start_tasks(per_class);
    }

    // Each worker owns a private buffer keyed by its stable tid and takes tasks
    // by index, flushing full batches (and the remainder on exit) through
    // consume_batch. Only consume_batch must be thread-safe (per tid).
    const unsigned nthreads = std::min<unsigned>(threads, static_cast<unsigned>(tasks.size()));
    std::vector<Stats> local(nthreads);
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    std::atomic<std::size_t> next_task{0};

    for (unsigned t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t]() {
            EmitBuf buf(cap);
            Ctx ctx{M, &opts, nullptr, 0, nullptr, chain, &buf, &consume_batch, t, cap};
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
                const std::uint64_t found0 = mine.survivors;
                dfs_task(ctx, mine, fr);
                progress::task_finished(fr.f, mine.survivors - found0);
            }
            flush_rest(consume_batch, t, buf);
            if (on_worker_done) {
                on_worker_done(t);
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
