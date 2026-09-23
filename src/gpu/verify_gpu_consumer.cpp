#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

namespace collatz::gpu::verify_gpu_detail {

// Clears a finished Job (capacity kept) into the pool the producers take their
// next vectors from. Freeing a finished Job's host vectors (about 185 MB at
// 2^20 survivors: nodes plus the SoA batch) would make each producer allocate
// and page-fault a fresh batch. A Job is recycled only after every command that
// read it has completed (its dispatch ended with a blocking read), so no
// transfer sees the reuse.
void GpuRun::recycle_job(Job& j) {
    j.batch.clear();
    j.nodes.clear();
    j.pub_max = collatz::u128_max;
    std::lock_guard<std::mutex> lk(qmtx);
    if (job_pool.size() < kJobPoolCap) {
        job_pool.push_back(std::move(j)); // else the caller frees it
    }
}

// The consumer (dispatch) thread: the device setup, which overlaps Step 1,
// then every batch in FIFO order, then whatever is still in flight. Any
// exception, from the setup on (an OpenCL error, a failed allocation),
// abandons the run, and the main thread rethrows it after the join.
void GpuRun::consumer_main() {
    try {
        // Device setup runs here while the producers already enumerate; the
        // first dispatch below follows it in program order.
        upload_setup();
        // Drains both queues when this block exits, by a throw too.
        const LaGuard la_guard{runner};
        dispatch_loop();
        if (!abandoned.load()) {
            drain_inflight();
        }
    } catch (...) {
        abandon(std::current_exception());
    }
}

// Keeps the run's first exception for the main thread and stops the pipeline:
// the queued batches are freed, the producers hand over no more batches
// (enqueue_job) and wake if they wait for queue space, and the consumer takes
// no more (dispatch_loop, pop_job). Called on the consumer, a producer or the
// main thread.
void GpuRun::abandon(std::exception_ptr e) noexcept {
    std::deque<Job> dropped;
    {
        std::lock_guard<std::mutex> lk(qmtx);
        if (!run_error) {
            run_error = std::move(e);
        }
        abandoned.store(true);
        dropped.swap(jobq);
    }
    cv_push.notify_all();
    cv_pop.notify_all();
}

// Pop the next batch for the lookahead without blocking; true on success.
bool GpuRun::try_pop_la() {
    {
        std::lock_guard<std::mutex> lk(qmtx);
        if (jobq.empty()) {
            return false;
        }
        la_job = std::move(jobq.front());
        jobq.pop_front();
        la_has = true;
    }
    // With producer prep, producers wait on cv_push for two different
    // conditions (slot reservation, push), so notify_one could wake one
    // that cannot proceed; wake them all.
    producer_prep ? cv_push.notify_all() : cv_push.notify_one();
    return true;
}

// Takes batches until the producers are done and the queue is empty, and
// submits each one behind the batch in flight.
void GpuRun::dispatch_loop() {
    for (;;) {
        if (abandoned.load(std::memory_order_relaxed)) {
            return; // a producer failed; nothing more is folded
        }
        if (!infl_pend.empty() && infl_redo.empty() && !la_has) {
            // Nothing to submit behind the in-flight batch yet: fold it rather
            // than block on the queue with it unfolded.
            bool empty_now = false;
            {
                std::lock_guard<std::mutex> lk(qmtx);
                empty_now = jobq.empty();
            }
            if (empty_now) {
                infl_finish_front();
                continue;
            }
        }
        Job job;
        if (!pop_job(job)) {
            break;
        }
        submit_inflight(job);
    }
}

// Takes the next batch: a split half or discarded batch first, then the
// lookahead batch, then the queue, waiting for the producers. Returns false
// when producing is done and the queue is empty, or the run is abandoned.
bool GpuRun::pop_job(Job& job) {
    if (!infl_redo.empty()) {
        job = std::move(infl_redo.front()); // a discarded batch or split half
        infl_redo.pop_front();
    } else if (la_has) {
        job = std::move(la_job); // already popped (and prefetched)
        la_job = Job{};
        la_has = false;
    } else {
        std::unique_lock<std::mutex> lk(qmtx);
        cv_pop.wait(lk, [&] { return !jobq.empty() || producing_done || abandoned.load(); });
        if (jobq.empty() || abandoned.load()) {
            return false; // producing done and queue drained, or the run abandoned
        }
        job = std::move(jobq.front());
        jobq.pop_front();
        if (first_pickup_s < 0.0) {
            first_pickup_s = since_start(); // startup-ordering diagnostic
        }
        lk.unlock();
        producer_prep ? cv_push.notify_all() : cv_push.notify_one(); // see try_pop_la
    }
    return true;
}

} // namespace collatz::gpu::verify_gpu_detail
