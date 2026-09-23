#include "collatz/frontier.hpp"
#include "collatz/progress.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

void GpuRun::run_pipeline() {
    init_producer_prep();
    std::thread consumer([this] { consumer_main(); });

    // The consumer is joined on every path: a std::thread destroyed while
    // joinable calls std::terminate. A throw here abandons the run.
    std::uint64_t masked_fmin_total = 0;
    try {
        producers.resize(build_threads == 0 ? 1u : build_threads);
        st = enumerate();

        // Flush any remainder the worker hook left and sum the f < fmin pruned
        // counts, which join masked_out only after the consumer exits so they
        // cannot race its reduce.
        for (Producer& p : producers) {
            masked_fmin_total += p.masked_fmin;
            flush_remainder(p);
        }
    } catch (...) {
        abandon(std::current_exception());
    }
    {
        std::lock_guard<std::mutex> lk(qmtx);
        producing_done = true;
    }
    cv_pop.notify_all();
    consumer.join();
    if (run_error) {
        std::rethrow_exception(run_error);
    }
    masked_out += masked_fmin_total;
}

// Hands producer p's batch to the consumer with its prefix bound.
inline void GpuRun::flush_producer(Producer& p) {
    const collatz::u128 pub = prefix_ub(p.pmax_hi, p.smax_hi, twoA - 1u);
    p.pmax_hi = 0;
    p.smax_hi = 0;
    p.flushed_once = true;
    enqueue_job(p.batch, p.nodes, pub);
}

// Flush all of producer p's buffered survivors (fewer than flush_cap: a
// producer flushes as soon as its batch reaches the cap).
inline void GpuRun::flush_remainder(Producer& p) {
    if (!p.nodes.empty()) {
        flush_producer(p);
    }
}

// Append a Step-1 survivor and its a* to producer p's batch.
inline void GpuRun::append_node(Producer& p, const collatz::step1::Node& node_in) {
    if (node_in.f < fmin) {
        p.masked_fmin += twoA; // f < fmin: every extension is pruned (mask == 0).
        return;
    }
    collatz::step1::Node node = node_in;
    if (block) {
        collatz::frontier::lift_survivor(node, lift);
    }
    cl_uint astar = twoA; // no partial-OEE pruning by default.
    if (opts.partial_oee) {
        // Use the a* carried down the Step-1 DFS (Node::oee_astar, enabled by
        // s1.track_oee_astar in enumerate), as the CPU mask() path does. The
        // kOeeAStarNone sentinel compares >= 2^A, which means no prune (astar
        // stays twoA).
        const collatz::u128 a_star = node.oee_astar;
        if (a_star < static_cast<collatz::u128>(twoA)) {
            astar = static_cast<cl_uint>(a_star);
        }
    }
    if (p.nodes.capacity() == 0) {
        // Take a finished batch's cleared vectors instead of allocating and
        // page-faulting a fresh batch.
        Job r;
        bool got = false;
        {
            std::lock_guard<std::mutex> lk(qmtx);
            if (!job_pool.empty()) {
                r = std::move(job_pool.back());
                job_pool.pop_back();
                got = true;
            }
        }
        if (got) {
            p.nodes.swap(r.nodes);
            std::swap(p.batch, r.batch);
            p.batch.clear();
            p.nodes.clear();
        }
        // Reserve a full batch once (virtual; pages touched as filled): no regrowth copies.
        p.nodes.reserve(flush_cap);
        p.batch.astar.reserve(flush_cap);
    }
    p.batch.astar.push_back(astar); // the other kernel inputs: build_inputs at flush
    p.nodes.push_back(node);
    p.pmax_hi = node.pmax > p.pmax_hi ? node.pmax : p.pmax_hi;
    p.smax_hi = node.smax > p.smax_hi ? node.smax : p.smax_hi;
    if (p.nodes.size() >= (p.flushed_once ? flush_cap : first_cap)) {
        flush_producer(p);
    }
}

// Step-1 enumeration on the CPU, overlapped with GPU dispatch. Each tid owns
// producers[tid], so the hot path takes no lock. With A > 0 and partial_oee the
// Step-1 DFS carries the partial-OEE a* (Node::oee_astar), as for the CPU
// verifier.
collatz::step1::Stats GpuRun::enumerate() {
    collatz::step1::Options s1 = opts.step1;
    s1.track_oee_astar = opts.A > 0 && opts.partial_oee;
    enum_start_s = since_start();
    return collatz::step1::enumerate_parallel_buffered(
        M,
        build_threads,
        s1,
        // A throw must not leave a Step-1 worker's thread, which would call
        // std::terminate, so both callbacks end the run through abandon. After
        // that the survivors are dropped.
        [&](unsigned tid, const collatz::step1::Node* ns, std::size_t cnt) {
            if (abandoned.load(std::memory_order_relaxed)) {
                return;
            }
            try {
                Producer& p = producers[tid];
                const std::uint64_t pruned0 = p.masked_fmin;
                for (std::size_t k = 0; k < cnt; ++k) {
                    append_node(p, ns[k]);
                }
                // --progress-file: a survivor with f < fmin has no candidate and
                // is finished here; the others when their batch has been folded.
                if (p.masked_fmin != pruned0) {
                    collatz::progress::survivors_done((p.masked_fmin - pruned0) / twoA);
                }
            } catch (...) {
                abandon(std::current_exception());
            }
        },
        // Each Step-1 worker flushes its own producer's remainder on its own
        // thread as soon as it runs out of tasks (enqueue_job is thread-safe per
        // producer), rather than the main thread flushing them all serially
        // after the join.
        collatz::step1::WorkerDoneHook([&](unsigned tid) {
            try {
                flush_remainder(producers[tid]);
            } catch (...) {
                abandon(std::current_exception());
            }
        }));
}

} // namespace collatz::gpu::verify_gpu_detail
