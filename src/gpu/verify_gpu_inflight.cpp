#include "collatz/progress.hpp"
#include "collatz/u128.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

namespace collatz::gpu::verify_gpu_detail {

// Per-batch bookkeeping for a finished in-flight batch. The next batch is
// already queued on the device, so the release is off the device's critical
// path.
void GpuRun::infl_retire(Job& job) {
    runner.prefetch_forget(job.batch); // before job is freed or recycled
    runner.retire_packed(job.batch);   // unused pack block back to the pool
    recycle_job(job);
}

// With an injection armed, a batch still in flight when a serial
// dispatch (re-run, split, fallback) validates first would validate out
// of the injection sequence it was drawn for; discard and resubmit it.
// Without an injection nothing is discarded: the in-order queue already
// orders the serial dispatch after it, and its slot is untouched.
void GpuRun::infl_discard_rest() {
    if (!runner.selfcheck_inject_armed()) {
        return;
    }
    while (!infl_pend.empty()) {
        InflPend& q = infl_pend.back();
        runner.cpi_discard(q.slot);
        infl_redo.push_front(std::move(q.job));
        infl_pend.pop_back();
    }
}

// Validate and fold the oldest in-flight batch (the other one keeps the
// device busy meanwhile). Self-check, retry, CPU fallback, flag-overflow
// retry and the split are process_job_cpi's, applied when the
// readback lands; every re-run is the serial run_cpi.
void GpuRun::infl_finish_front() {
    InflPend p = std::move(infl_pend.front());
    infl_pend.pop_front();
    Job& job = p.job;
    collatz::u128 Rlb = p.rlb;
    CpiResult cr;
    const auto t_r0 = Clock::now();
    const auto serial = [&] {
        infl_discard_rest();
        runner.run_cpi(job.batch, kc, Rlb, kCpiFlagCap, kCpiWg, cr);
    };
    const auto ok = [&] { return sc_why_cpi(cr); };
    bool sc_ok = sc_run([&] { runner.cpi_complete(p.slot, cr); }, serial, ok);
    if (sc_ok && cr.flag_overflow && capped(cr.max_bs) > Rlb) {
        // Retry once with this batch's own block-start max as the bound, kept
        // only if the flagged excursions bring the record up to it (see
        // dispatch_cpi).
        ++cpi_retry_batches;
        Rlb = capped(cr.max_bs);
        sc_ok = sc_run(serial, serial, ok);
        if (sc_ok && !cr.flag_overflow && !retry_bound_holds(job, cr, Rlb)) {
            Rlb = flag_bound();
            sc_ok = sc_run(serial, serial, ok);
        }
    }
    t_run_s += std::chrono::duration<double>(Clock::now() - t_r0).count();
    if (!sc_ok || cr.flag_overflow) {
        infl_discard_rest(); // the CPU / split / per-survivor paths follow
    }
    if (sc_ok && cr.cand_overflow && job.batch.size() > 1) {
        // The same split by survivors as split_cpi_job, but the halves go to
        // the front of infl_redo, so
        // the loop submits them through cpi_submit (two in flight, uploads
        // on queue_ without a prefetch) instead of a serial run_cpi, which
        // waits for the other in-flight batch and drains the pipeline. The
        // overflowed attempt folded nothing (cpi_complete returns before
        // any fold on cand_overflow), so each half folds once; a half that
        // overflows again splits again, which terminates because a half of
        // <= cap / 2^A survivors cannot overflow. With an injection armed,
        // infl_discard_rest has already moved the other in-flight batch to
        // infl_redo and the halves go in front of it, so the validation
        // order lo, hi, rest matches the serial split, and so does the result.
        ++cpi_split_batches;
        const std::size_t n = job.batch.size();
        const std::size_t mid = n / 2;
        Job lo = sub_job(job, 0, mid);
        Job hi = sub_job(job, mid, n);
        infl_redo.push_front(std::move(hi));
        infl_redo.push_front(std::move(lo));
        ++split_pipelined_halves;
        infl_retire(job);
        return;
    }
    finish_cpi(job, cr, sc_ok);
    collatz::progress::survivors_done(job.nodes.size()); // --progress-file
    infl_retire(job);
}

// Submits a batch behind the one in flight and folds the older one while this
// one runs.
void GpuRun::submit_inflight(Job& job) {
    // The flag bound is the record as of the last folded batch, so it lags the
    // batch in flight. That is sound: the device flags a candidate when its
    // tail bound reaches Rlb, every flagged candidate is re-walked exactly on
    // the host (settle_candidate), and the prefix-bound re-walk in finish_cpi
    // uses the current record. Rlb only has to stay <= the true global max so
    // that no candidate that can equal the max goes unflagged; a record from
    // fewer folded batches is no larger, so it can only add flags, never drop
    // one. The folds do not depend on Rlb, and max and argmax come from the
    // exact re-walks, so the lag does not change the Result. A lower Rlb can
    // overflow the flag list more often; such a batch takes the usual
    // Rlb = max_bs retry or the per-survivor fallback.
    collatz::u128 Rlb = flag_bound();
    const int slot = runner.cpi_submit(job.batch, kc, Rlb, kCpiFlagCap, kCpiWg);
    if (slot < 0) {
        // Out of the pipeline's scope: drain it, then run serially.
        while (!infl_pend.empty()) {
            infl_finish_front();
        }
        process_job_cpi(job);
        collatz::progress::survivors_done(job.nodes.size());
        infl_retire(job);
        return;
    }
    infl_pend.push_back(InflPend{std::move(job), slot, Rlb});
    if (infl_pend.size() == 2 && runner.copy_queue_ready() && !la_has && infl_redo.empty() &&
        try_pop_la()) {
        // Pop the batch after next and pack its inputs into a staging slot
        // while the device still holds two batches, so between the older
        // batch's completion and the next submit the host only folds and
        // enqueues the copy. la_job owns the vectors, as for a prefetch.
        runner.prefetch_prepack(la_job.batch);
    }
    if (infl_pend.size() == 2) {
        infl_finish_front();
    }
    if (infl_pend.size() <= 1 && runner.copy_queue_ready() && infl_redo.empty() &&
        (la_has || try_pop_la())) {
        // Early prefetch: the older batch has been waited on and folded, so
        // only the batch just submitted is in flight, and the input set the
        // prefetch overwrites (last read by the older batch or before it) has
        // no reader left. The copy of the batch after next goes into the idle
        // input set with no cross-queue wait and overlaps the submitted
        // batch's mask and tail. la_job may already hold a batch packed above;
        // the runner skips a batch it has already prefetched. If
        // infl_finish_front pushed split halves or discards onto infl_redo, no
        // prefetch is issued.
        runner.prefetch_inputs(la_job.batch);
    }
}

// Folds every batch still in flight, resubmitting a discarded one serially.
void GpuRun::drain_inflight() {
    while (!infl_pend.empty() || !infl_redo.empty()) {
        if (infl_pend.empty()) {
            // Only after a discard at the end of the run: resubmit serially.
            Job j = std::move(infl_redo.front());
            infl_redo.pop_front();
            process_job_cpi(j);
            collatz::progress::survivors_done(j.nodes.size());
            infl_retire(j);
            continue;
        }
        infl_finish_front();
    }
}

} // namespace collatz::gpu::verify_gpu_detail
