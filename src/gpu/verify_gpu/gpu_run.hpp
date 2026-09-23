#ifndef COLLATZ_GPU_VERIFY_GPU_GPU_RUN_HPP
#define COLLATZ_GPU_VERIFY_GPU_GPU_RUN_HPP

// GpuRun, one GPU verification run: its member functions, defined in the
// verify_gpu_*.cpp files, and the inline ones that several of those files call.

#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run_state.hpp"
#include "verify_gpu/internal.hpp"
#include "verify_gpu/records.hpp"
#include "verify_gpu/run_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

namespace collatz::gpu::verify_gpu_detail {

// One GPU verification run, once verify_gpu has the device, the preflight
// folds and the Step-2 filter: the Step-1 producer threads, the consumer
// (dispatch) thread that owns the OpenCL queue and the run-wide folds, then the
// Result and the diagnostics.
class GpuRun : GpuRunState {
public:
    using GpuRunState::GpuRunState;

    // Runs the producers and the consumer, which uploads the device tables
    // first, and joins them.
    void run_pipeline();
    // Fills the Result and prints the run summaries and reports.
    void finish();
    // The diagnostics verify_gpu returns in *msg.
    [[nodiscard]] std::string summary_message() const;

private:
    void upload_setup();
    void init_producer_prep();

    [[nodiscard]] collatz::u128 capped(collatz::u128 v) const;
    [[nodiscard]] collatz::u128 flag_bound() const;
    [[nodiscard]] double since_start() const;
    void add_elapsed(double& acc, Clock::time_point t0);
    void recycle_job(Job& j);

    template <class First, class Retry, class Ok>
    bool sc_run(First&& first, Retry&& retry, Ok&& ok);
    [[nodiscard]] static const char* sc_why_cpi(const CpiResult& r);

    [[nodiscard]] collatz::u128 cand_n(const collatz::step1::Node& node, unsigned a) const;
    [[nodiscard]] Replay
    replay(const collatz::step1::Node& node, unsigned a, collatz::u128 n) const;
    void fold_replay(const Replay& r);
    void settle_candidate(const collatz::step1::Node& node, unsigned a);
    void rewalk_candidate(const collatz::step1::Node& node, unsigned a);

    void process_job(Job& job);
    void flush_cpu_tail(CpuAcc& acc, CpuScratch& sc) const;
    void process_chunk_cpu(const std::vector<collatz::step1::Node>& nodes,
                           std::size_t begin,
                           std::size_t end,
                           CpuAcc& acc,
                           CpuScratch& sc) const;
    void process_job_cpu_exact(Job& job);
    void split_cpi_job(Job& job);
    template <class Run>
    bool dispatch_cpi(const Job& job, CpiResult& cr, Run&& run);
    [[nodiscard]] bool retry_bound_holds(const Job& job, const CpiResult& cr, collatz::u128 rlb);
    void process_job_cpi(Job& job);
    void finish_cpi(Job& job, CpiResult& cr, bool sc_ok);
    void fold_cpi_batch(Job& job, const CpiResult& cr);

    void consumer_main();
    void abandon(std::exception_ptr e) noexcept;
    bool try_pop_la();
    void infl_retire(Job& job);
    void infl_discard_rest();
    void infl_finish_front();
    void dispatch_loop();
    bool pop_job(Job& job);
    void submit_inflight(Job& job);
    void drain_inflight();

    void build_inputs(SurvivorBatch& b, const std::vector<collatz::step1::Node>& ns) const;
    void
    enqueue_job(SurvivorBatch& b, std::vector<collatz::step1::Node>& ns, collatz::u128 pub_max);
    void flush_producer(Producer& p);
    void flush_remainder(Producer& p);
    void append_node(Producer& p, const collatz::step1::Node& node_in);
    [[nodiscard]] collatz::step1::Stats enumerate();

    void fill_result();
    void print_run_stats();
    [[nodiscard]] std::string mode_lines() const;
    [[nodiscard]] std::string startup_lines() const;
};

inline collatz::u128 GpuRun::capped(collatz::u128 v) const {
    return v < bar_cap ? v : bar_cap;
}

// Flag bound for the next dispatch: the exact record, capped at bar_cap when a
// record list is on. Every record value is an excursion a re-walk computed, so
// the bound never exceeds the final record. A device block-start maximum is not
// used: a block start can follow a mid-block drop below its n, so it need not
// be below any excursion (retry_bound_holds checks it before a retry uses it).
inline collatz::u128 GpuRun::flag_bound() const {
    return capped(rec.have ? rec.maxv : static_cast<collatz::u128>(0));
}

inline double GpuRun::since_start() const {
    return std::chrono::duration<double>(Clock::now() - t_start).count();
}

// Adds the seconds since t0 to the consumer timer `acc`.
inline void GpuRun::add_elapsed(double& acc, Clock::time_point t0) {
    acc += std::chrono::duration<double>(Clock::now() - t0).count();
}

// Runs first() then, while ok() reports a failure, retry(); returns false
// when all 1 + kScReruns attempts failed.
template <class First, class Retry, class Ok>
bool GpuRun::sc_run(First&& first, Retry&& retry, Ok&& ok) {
    const std::uint64_t id = sc_batches++;
    first();
    for (int attempt = 0;; ++attempt) {
        const char* why = ok();
        if (why == nullptr) {
            return true;
        }
        if (attempt == kScReruns) {
            ++sc_cpu_fallback;
            std::fprintf(stderr,
                         "gpu selfcheck: batch %llu failed %d times (%s); computing it on "
                         "the CPU\n",
                         static_cast<unsigned long long>(id),
                         attempt + 1,
                         why);
            return false;
        }
        ++sc_retried;
        std::fprintf(stderr,
                     "gpu selfcheck: batch %llu failed (%s); re-running\n",
                     static_cast<unsigned long long>(id),
                     why);
        retry();
    }
}

} // namespace collatz::gpu::verify_gpu_detail

#endif // COLLATZ_GPU_VERIFY_GPU_GPU_RUN_HPP
