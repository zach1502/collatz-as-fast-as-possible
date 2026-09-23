#ifndef COLLATZ_GPU_VERIFY_GPU_GPU_RUN_STATE_HPP
#define COLLATZ_GPU_VERIFY_GPU_GPU_RUN_STATE_HPP

// GpuRunState: the data members of GpuRun (gpu_run.hpp) and their initializers.

#include "collatz/frontier.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "host_pipeline.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/internal.hpp"
#include "verify_gpu/knobs.hpp"
#include "verify_gpu/records.hpp"
#include "verify_gpu/run_types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

// bv_words32_per, here and in plan_device_memory, computes size_t{1} << B with
// B up to 32 (step2::max_B), which a 32-bit size_t cannot hold.
static_assert(sizeof(std::size_t) == 8, "the --gpu path needs a 64-bit size_t");

// The data members are grouped by the threads that write them while the threads run.
struct GpuRunState {
    GpuRunState(const collatz::verify::Options& opts_in,
                SurvivorRunner& runner_in,
                const collatz::step2::Filter& filter_in,
                collatz::verify::Result& res_in,
                const RunStart& start,
                PreflightFolds&& pre);

    // Run inputs and settings, fixed before the threads start and read by all
    // of them. The initializers run in declaration order.
    const collatz::verify::Options& opts;
    SurvivorRunner& runner;
    const collatz::step2::Filter& filter;
    collatz::verify::Result& res;
    const Clock::time_point t_start;
    const unsigned build_threads;
    const double t_preflight_s;
    const double filter_seconds;
    const collatz::u128 preflimit;
    const int num_bv_buffers;
    const cl_uint buf_words_log2;
    const bool block = opts.block_h != 0;
    const unsigned M = opts.N - opts.A;
    const RecordList rl = record_list(opts);
    // Every flag bound and record gate is capped at bar_cap (record_list).
    const collatz::u128 bar_cap = record_bar_cap(rl);
    const unsigned fmin = filter.fmin();
    const unsigned nbv = opts.num_bitvectors;
    const cl_uint twoA = 1u << opts.A;
    const std::size_t bv_words32_per = (static_cast<std::size_t>(1) << opts.B) / 32u;
    const SurvivorConst kc = kernel_const(opts, M, twoA, filter, preflimit);
    // Batch ramp. With a fixed 2^20 cap, an N=40 producer holds all its
    // survivors until Step 1 ends, so the device idles through the enumeration
    // and then gets every producer's remainder at once. With the ramp, a
    // producer's first flush fires at flush_cap >> 3, so the device starts
    // early, and later batches stay full size. Only batch boundaries move, and
    // every Result fold is schedule-invariant. flush_cap is at most 2^(32-A)
    // survivors (batch_survivor_cap), so the packed candidate ids of every
    // batch fit 32 bits: a producer flushes at the cap, its remainder is
    // smaller, and a split half is smaller than its batch.
    const std::size_t flush_cap = batch_survivor_cap(opts.A, gpu_flush_cap());
    const std::size_t first_cap = std::max<std::size_t>(1, flush_cap >> 3);
    // Frontier block mode: each Step-1 survivor is lifted into the block before
    // anything reads it (frontier::lift_survivor, as verify::run does), so the
    // uploaded n0/m/a* and every re-walk see the block member L + a*2^M + n0
    // itself.
    const collatz::frontier::Lift lift = collatz::frontier::make_lift(opts.block_h, opts.N, opts.A);
    const std::array<std::uint32_t, 1u << 16>& tbl16_host = host_tbl16();
    const std::vector<collatz::step1::Node> kNoNodes;
    bool producer_prep = false; // producer-side pack pool (init_producer_prep)
    // Test hook (COLLATZ_GPU_INJECT_PRODUCER_FAIL=<k>, k >= 1): the k-th batch a
    // producer hands over throws on that producer's thread (handed_batches
    // counts them). 0 is off.
    const std::size_t producer_fail_at = env_size_or("COLLATZ_GPU_INJECT_PRODUCER_FAIL", 0);
    // One entry per Step-1 worker (keyed by tid), written by that worker; sized
    // when the producers start.
    std::vector<Producer> producers;

    // Shared by the producers and the consumer; guarded by qmtx unless atomic. alignas keeps these
    // writes off the cache lines of the settings above and of the consumer state below.
    alignas(128) std::mutex qmtx;
    std::condition_variable cv_pop;  // consumer waits for a job
    std::condition_variable cv_push; // producers wait for queue space
    std::deque<Job> jobq;
    bool producing_done = false;
    std::vector<Job> job_pool;                // guarded by qmtx
    std::size_t pp_reserved = 0;              // queue slots held by packing producers (under qmtx)
    std::atomic<double> producer_pack_s{0.0}; // total producer-side pack time
    // The first exception of the consumer or of a producer (abandon), rethrown
    // on the main thread after the join; guarded by qmtx. `abandoned` is set
    // with it, under qmtx, and stops the producers from handing over batches
    // and the consumer from taking them.
    std::exception_ptr run_error;
    std::atomic<bool> abandoned{false};
    std::atomic<std::uint64_t> handed_batches{0}; // only with producer_fail_at

    // Consumer thread state. The main thread writes it only before the consumer
    // starts and after the join. The fold accumulators (thread-count
    // independent; wrapping/mod sums) start from the preflight's folds.
    alignas(128) std::uint64_t coverage; // mod kCoveragePrime
    std::uint64_t pow_digest;            // pow result digest, mod kCoveragePrime
    std::uint64_t blocks_total;          // diagnostic: wrapping u64 sum of block counts
    std::uint64_t candidates;            // preflight + processed survivor candidates
    std::uint64_t masked_out = 0;        // Step-2 pruned extensions
    MaxRecord rec;                       // exact global max + smallest-n argmax
    // Phase timers (host wall). Only the consumer thread writes them, and the
    // main thread reads them after the join, so plain doubles are safe.
    double t_run_s = 0.0;                     // dispatch+wait wall (kernel + transfer)
    double t_reduce_s = 0.0;                  // per-batch fold reduction wall
    double t_rewalk_s = 0.0;                  // flagged record-contender CPU re-walk wall
    std::uint64_t rewalk_items = 0;           // survivors that hit the flag (re-walked)
    std::uint64_t total_items = 0;            // survivors dispatched to the device
    std::uint64_t cpi_fallback_batches = 0;   // batches that overflowed the flag list
    std::uint64_t cpi_retry_batches = 0;      // overflowed batches re-dispatched with Rlb=max_bs
    std::uint64_t cpi_split_batches = 0;      // candidate-buffer overflows split in half
    std::uint64_t split_pipelined_halves = 0; // of those, splits resubmitted in flight
    std::uint64_t cpi_batches = 0;            // batches processed by the CPI path
    std::uint64_t cpi_tail_flagged = 0;       // device tail-flagged candidates re-walked
    std::uint64_t cpi_prefix_survivors = 0;   // host prefix-flagged survivors re-walked
    SurvivorOut out;                          // consumer-owned scratch (process_job)
    CpuScratch fallback_scratch;              // process_job_cpu_exact
    // Per-batch self-check (consumer thread only). Every device dispatch
    // returns nonce-keyed records that the runner verifies (sc_ok); a failed
    // batch is re-run from its inputs up to kScReruns times, then computed on
    // the CPU (process_job_cpu_exact).
    std::uint64_t sc_batches = 0;
    std::uint64_t sc_retried = 0;
    std::uint64_t sc_cpu_fallback = 0;
    std::uint64_t sc_unchecked = 0;
    double t_upload_s = 0.0;
    double t_upload_end_s = 0.0;  // since t_start (startup-ordering diagnostic)
    double first_pickup_s = -1.0; // since t_start: consumer takes batch 0
    std::size_t upload_chunk_count = 0;
    // Copy-queue lookahead: while batch i's mask and tail run, the in-flight
    // loop (submit_inflight) pops batch i+1 and has its inputs copied into the
    // idle device input set on the copy queue. Batches stay in FIFO order, so
    // the lookahead changes neither the folds nor the Rlb sequence. la_job owns
    // batch i+1's vectors until the next iteration moves them into `job` (data
    // pointers survive the move), and that batch's blocking reads are ordered
    // after the copy. LaGuard drains both queues when the consumer exits.
    Job la_job;
    bool la_has = false;
    std::deque<InflPend> infl_pend; // submitted, not yet validated/folded
    std::deque<Job> infl_redo;      // discarded batches and split halves: submitted first

    // Main thread, while the producers run and after the join.
    double enum_start_s = 0.0; // since t_start: Step-1 producers start
    collatz::step1::Stats st{};
};

} // namespace collatz::gpu::verify_gpu_detail

#endif // COLLATZ_GPU_VERIFY_GPU_GPU_RUN_STATE_HPP
