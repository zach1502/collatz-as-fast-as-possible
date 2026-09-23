#include "collatz/gpu/opencl.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "host_pipeline.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

namespace {

inline void atomic_add_seqcst(std::atomic<double>& a, double v) noexcept {
    double expected = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(
        expected, expected + v, std::memory_order_seq_cst, std::memory_order_relaxed)) {
    }
}

} // namespace

// Fill b's per-survivor kernel inputs (all but astar) from ns, in order.
void GpuRun::build_inputs(SurvivorBatch& b, const std::vector<collatz::step1::Node>& ns) const {
    const std::size_t n = ns.size();
    b.n0.resize(n);
    b.m.resize(n);
    b.p3f.resize(n);
    b.inv3f.resize(n);
    b.bvbase.resize(n);
    b.bvmode.resize(n);
    b.f0.resize(n);
    cl_uint n0_hi = 0u; // upload-trim hints, re-checked by the runner
    cl_uint m_hi = 0u;
    for (std::size_t s = 0; s < n; ++s) {
        const collatz::step1::Node& node = ns[s];
        const unsigned i = node.f - fmin; // append_node keeps only f >= fmin
        cl_uint bvmode = 0;
        cl_uint bvbase = 0;
        cl_uint inv3f = 0;
        if (i >= nbv) {
            bvmode = 1; // no lookahead bitvector: all-ones window.
        } else {
            bvbase = static_cast<cl_uint>(static_cast<std::size_t>(i) * bv_words32_per);
            inv3f = static_cast<cl_uint>(filter.inv3f(i));
        }
        b.n0[s] = to_uint4(node.n0);
        b.m[s] = to_uint4(node.m);
        b.p3f[s] = to_uint4(collatz::pow3(node.f));
        b.inv3f[s] = inv3f;
        b.bvbase[s] = bvbase;
        b.bvmode[s] = bvmode;
        b.f0[s] = static_cast<cl_uint>(node.f);
        n0_hi |= b.n0[s].s[2] | b.n0[s].s[3];
        m_hi |= b.m[s].s[2] | b.m[s].s[3];
    }
    b.hint_n0_hi = n0_hi;
    b.hint_m_hi = m_hi;
}

// Hand a full batch to the consumer, blocking while the queue is at cap
// (backpressure bounds resident memory). Leaves b/ns empty for reuse. Once the
// run is abandoned (abandon) the batch is dropped instead; `abandoned` is set
// under qmtx, so no batch is queued after it.
void GpuRun::enqueue_job(SurvivorBatch& b,
                         std::vector<collatz::step1::Node>& ns,
                         collatz::u128 pub_max) {
    if (producer_fail_at != 0 && handed_batches.fetch_add(1) + 1 == producer_fail_at) {
        throw std::runtime_error("injected producer failure (COLLATZ_GPU_INJECT_PRODUCER_FAIL)");
    }
    const auto drop = [&b, &ns] {
        b.clear();
        ns.clear();
    };
    if (abandoned.load(std::memory_order_relaxed)) {
        drop();
        return;
    }
    // The per-survivor kernel inputs, in batch order.
    build_inputs(b, ns);
    // The candidate upper bound cpi_submit needs, summed here on the producer,
    // off the single dispatch thread. It is the same loop and value as in
    // cpi_submit; see SurvivorBatch.
    b.cupper_hint = cand_upper_bound(
        b.astar.begin(), b.astar.begin() + static_cast<std::ptrdiff_t>(ns.size()), twoA);
    b.cupper_two_a = twoA;
    std::unique_lock<std::mutex> lk(qmtx);
    if (producer_prep && !ns.empty()) {
        // Reserve a queue slot first, then pack outside the lock. A producer
        // waiting on a full queue therefore never holds a pool block, so the
        // blocks in use are bounded by the queue cap plus the consumer's
        // in-flight batches (the pool size), not by the number of blocked
        // producers. The queue plus all outstanding reservations never
        // exceed kMaxInFlight, so the push after the pack does not block.
        cv_push.wait(lk,
                     [&] { return abandoned.load() || jobq.size() + pp_reserved < kMaxInFlight; });
        if (abandoned.load()) {
            lk.unlock();
            drop();
            return;
        }
        ++pp_reserved;
        lk.unlock();
        const auto pk0 = Clock::now();
        runner.pack_on_producer(b);
        const auto pk1 = Clock::now();
        lk.lock();
        --pp_reserved;
        atomic_add_seqcst(producer_pack_s, std::chrono::duration<double>(pk1 - pk0).count());
    }
    cv_push.wait(lk, [&] { return abandoned.load() || jobq.size() < kMaxInFlight; });
    if (abandoned.load()) {
        lk.unlock();
        drop();
        return;
    }
    jobq.push_back(Job{std::move(b), std::move(ns), pub_max});
    lk.unlock();
    cv_pop.notify_one();
    b.clear(); // NOLINT(bugprone-use-after-move): clear() resets the moved-from batch
    ns.clear();
}

} // namespace collatz::gpu::verify_gpu_detail
