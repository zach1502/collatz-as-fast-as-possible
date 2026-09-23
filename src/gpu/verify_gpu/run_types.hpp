#ifndef COLLATZ_GPU_VERIFY_GPU_RUN_TYPES_HPP
#define COLLATZ_GPU_VERIFY_GPU_RUN_TYPES_HPP

// The run's start parameters and the batch types that the producers and the
// consumer pass between them.

#include "collatz/gpu/opencl.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

// Device layout of the Step-2 bitvector table, from plan_device_memory.
struct BvLayout {
    int num_buffers = 1;
    cl_uint buf_words_log2 = 0;
};

// Defined in verify_gpu_setup.cpp.
[[nodiscard]] std::string plan_device_memory(SurvivorRunner& runner,
                                             const collatz::verify::Options& opts,
                                             collatz::verify::Result& res,
                                             BvLayout& layout);

// What verify_gpu has settled when the pipeline starts.
struct RunStart {
    Clock::time_point t_start;
    unsigned build_threads = 1;
    double t_preflight_s = 0.0;
    double filter_seconds = 0.0;
    collatz::u128 preflimit = 1;
    BvLayout layout;
};

// Step-1 enumeration threads build per-thread survivor batches and hand full
// ones to one consumer thread, which owns the OpenCL queue and all reduction
// state (folds, rec, timers), so the kernels overlap enumeration and batch
// building. The order does not change the result: the folds are commutative,
// every candidate that could hold the true maximum has ub >= Rlb for every
// Rlb and is re-walked in any order, and rec.offer's (max, smallest n) pick
// does not depend on order.
struct Job {
    SurvivorBatch batch;
    std::vector<collatz::step1::Node> nodes;
    // >= prefix_ub(pmax, smax, 2^A-1) of every node (a monotone bound).
    collatz::u128 pub_max = collatz::u128_max;
};

// Survivors [lo, hi) of `src` as a standalone job: `batch` (SoA per-survivor
// inputs), `nodes` (flag re-walks, prefix bound) and `pub_max`, a bound over
// the parent's nodes and so over any subset.
[[nodiscard]] inline Job sub_job(const Job& src, std::size_t lo, std::size_t hi) {
    Job j;
    const auto cut = [lo, hi](const auto& v) {
        using V = std::decay_t<decltype(v)>;
        return v.size() >= hi ? V(v.begin() + static_cast<std::ptrdiff_t>(lo),
                                  v.begin() + static_cast<std::ptrdiff_t>(hi))
                              : V{};
    };
    j.batch.n0 = cut(src.batch.n0);
    j.batch.m = cut(src.batch.m);
    j.batch.p3f = cut(src.batch.p3f);
    j.batch.astar = cut(src.batch.astar);
    j.batch.inv3f = cut(src.batch.inv3f);
    j.batch.bvbase = cut(src.batch.bvbase);
    j.batch.bvmode = cut(src.batch.bvmode);
    j.batch.f0 = cut(src.batch.f0);
    // The hints stay valid: a sub-range's high limbs are a subset of the
    // whole batch's.
    j.batch.hint_n0_hi = src.batch.hint_n0_hi;
    j.batch.hint_m_hi = src.batch.hint_m_hi;
    j.nodes = cut(src.nodes);
    j.pub_max = src.pub_max;
    return j;
}

// A two-deep pipeline entry: a batch submitted with cpi_submit and not yet
// validated or folded.
struct InflPend {
    Job job;
    int slot = -1;
    collatz::u128 rlb = 0; // the flag bound the batch was submitted with
};

// Drains both queues when the consumer exits, so no copy still reads a batch
// that is being freed.
struct LaGuard {
    SurvivorRunner& r;
    ~LaGuard() {
        r.drain_transfers();
    }
};

// Per-producer batch buffers (keyed by the stable tid) so producers build
// batches in parallel with no shared-state lock on the hot path.
struct Producer {
    SurvivorBatch batch;
    std::vector<collatz::step1::Node> nodes;
    std::uint64_t masked_fmin = 0; // f < fmin extensions (all pruned).
    collatz::u128 pmax_hi = 0;     // max node.pmax over the building batch
    collatz::u128 smax_hi = 0;     // max node.smax over the building batch
    bool flushed_once = false;     // false until the first (ramp-cap) flush
};

} // namespace collatz::gpu::verify_gpu_detail

#endif // COLLATZ_GPU_VERIFY_GPU_RUN_TYPES_HPP
