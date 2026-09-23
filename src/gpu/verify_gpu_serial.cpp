#include "collatz/u128.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <chrono>
#include <cstddef>

namespace collatz::gpu::verify_gpu_detail {

const char* GpuRun::sc_why_cpi(const CpiResult& r) {
    return r.sc_ok ? nullptr : (r.sc_why != nullptr ? r.sc_why : "unknown");
}

// A batch whose candidates overflow the id buffer (cpi_cand_hard_cap: 2^29
// ids, clamped to CL_DEVICE_MAX_MEM_ALLOC_SIZE, lowered by
// COLLATZ_GPU_CPI_CAND_CAP), or that holds more than 2^(32-A) survivors
// (run_cpi), is split by survivors, and each half runs on the CPI path,
// recursing if a half still overflows. This terminates: a half of at most
// min(cap / 2^A, 2^(32-A)) survivors fits both limits. The folds are
// commutative and every half re-walks its own flagged candidates, so the
// result equals the unsplit batch's.
void GpuRun::split_cpi_job(Job& job) {
    ++cpi_split_batches;
    const std::size_t n = job.batch.size();
    const std::size_t mid = n / 2;
    Job lo = sub_job(job, 0, mid);
    Job hi = sub_job(job, mid, n);
    process_job_cpi(lo);
    process_job_cpi(hi);
}

// Dispatches one CPI batch through the self-check, then retries once with the
// batch's own block-start max as the flag bound when its flag list overflowed.
// That bound is kept only if the flagged excursions bring the record up to it
// (retry_bound_holds); otherwise the batch is dispatched once more with the
// record. `run(rlb)` performs one dispatch into `cr`. Returns the self-check
// verdict.
template <class Run>
bool GpuRun::dispatch_cpi(const Job& job, CpiResult& cr, Run&& run) {
    // Lower bound on the final record, frozen before dispatch (drives the
    // device flag).
    collatz::u128 Rlb = flag_bound();
    const auto t_r0 = Clock::now();
    const auto dispatch = [&] { run(Rlb); };
    const auto ok = [&] { return sc_why_cpi(cr); };
    bool sc_ok = sc_run(dispatch, dispatch, ok);
    if (sc_ok && cr.flag_overflow && capped(cr.max_bs) > Rlb) {
        ++cpi_retry_batches;
        Rlb = capped(cr.max_bs);
        sc_ok = sc_run(dispatch, dispatch, ok);
        if (sc_ok && !cr.flag_overflow && !retry_bound_holds(job, cr, Rlb)) {
            Rlb = flag_bound();
            sc_ok = sc_run(dispatch, dispatch, ok);
        }
    }
    t_run_s += std::chrono::duration<double>(Clock::now() - t_r0).count();
    return sc_ok;
}

// Candidate-per-item (CPI) path: device dispatch, host fold reduction, and
// exact max recovery from the device's tail-flagged candidates and from the
// survivors whose host prefix bound reaches the record. A batch whose flag
// list overflows falls back to the per-survivor kernel (process_job). The
// result equals process_job's: the folds are commutative, and every
// candidate whose excursion could equal the global max is re-walked (a
// tail-dominated one is tail-flagged for any Rlb <= max; for a
// prefix-dominated one, its survivor's prefix bound reaches Rlb_now <= max).
void GpuRun::process_job_cpi(Job& job) {
    SurvivorBatch& batch = job.batch;
    if (batch.size() == 0) {
        return;
    }
    CpiResult cr;
    const bool sc_ok = dispatch_cpi(job, cr, [&](collatz::u128 rlb) {
        runner.run_cpi(batch, kc, rlb, kCpiFlagCap, kCpiWg, cr);
    });
    finish_cpi(job, cr, sc_ok);
}

// The post-dispatch half of process_job_cpi: CPU fallback, split,
// flag-overflow fallback, or fold and exact re-walk of the contenders. The
// in-flight loop dispatches differently but finishes every batch here too.
void GpuRun::finish_cpi(Job& job, CpiResult& cr, bool sc_ok) {
    SurvivorBatch& batch = job.batch;
    if (!sc_ok) {
        process_job_cpu_exact(job);
        return;
    }
    if (cr.cand_overflow && batch.size() > 1) {
        split_cpi_job(job);
        return;
    }
    if (cr.flag_overflow) {
        // Flag list overflowed: exact per-survivor kernel for this batch.
        ++cpi_fallback_batches;
        process_job(job);
        return;
    }
    fold_cpi_batch(job, cr);
}

} // namespace collatz::gpu::verify_gpu_detail
