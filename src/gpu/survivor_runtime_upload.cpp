#include "survivor_runtime/internal.hpp"

#include <cstdint>
#include <cstring>

namespace collatz::gpu {

using namespace survivor_detail;

// Uploads `batch`'s run_cpi inputs into `set` on queue `q` with the given trim
// bits. Returns the last write's event in *ev when ev != nullptr (the caller
// releases it).
void SurvivorRunner::upload_inputs(cl_command_queue q,
                                   const InputSet& set,
                                   const SurvivorBatch& batch,
                                   cl_uint& bits,
                                   cl_event* ev,
                                   bool skip_n0) {
    const std::size_t n = batch.size();
    StagedWrite w[8];
    int nw = 0;
    StagingSlot* st = nullptr;
    PackedBlock ppb; // the producer-packed block, when ppb.block >= 0
    if (pack_pool_ != nullptr && take_packed(batch, bits, ppb)) {
        // A producer packed this batch into pool block ppb.block with the same
        // trim bits this upload tries. The block stays InFlight, never handed
        // out again, until the last write below has completed.
        nw = ppb.nw;
        const StagedWrite* bw = packed_writes(ppb.block);
        for (int i = 0; i < nw; ++i) {
            w[i] = bw[i];
        }
        bits = ppb.bits;
    } else if (q == xfer_.copy_q && prepack_matches(batch)) {
        // Packed earlier by prefetch_prepack into stage[slot].
        // That slot has not been handed out since (next_staging invalidates the
        // pre-pack when it is), so its bytes are this batch's.
        PrePack& pp = xfer_.prepack;
        st = &xfer_.stage[pp.slot];
        nw = pp.nw;
        for (int i = 0; i < nw; ++i) {
            w[i] = pp.w[i];
        }
        bits = pp.bits;
        pp.valid = false;
        ++pipe_.prepack_hits;
    } else {
        st = &next_staging(staged_full_bytes(n));
        nw = pack_staged(batch, bits, st->host, w);
    }
    cl_event last = nullptr;
    for (int i = 0; i < nw; ++i) {
        if (skip_n0 && i == 0) {
            continue; // test hook: a lost input copy (stale device n0)
        }
        const bool is_last = i == nw - 1;
        check(clEnqueueWriteBuffer(q,
                                   set.*(w[i].buf),
                                   CL_FALSE,
                                   0,
                                   w[i].bytes,
                                   w[i].host,
                                   0,
                                   nullptr,
                                   (is_last && (st != nullptr || ppb.block >= 0 || ev != nullptr))
                                       ? &last
                                       : nullptr),
              w[i].what);
    }
    if (st != nullptr) {
        st->ev = last; // the slot is busy until this write completes
        if (ev != nullptr && last != nullptr) {
            clRetainEvent(last);
        }
    } else if (ppb.block >= 0) {
        set_packed_event(ppb.block, last); // the block is busy until this write completes
        if (ev != nullptr && last != nullptr) {
            clRetainEvent(last);
        }
    }
    if (ev != nullptr) {
        *ev = last;
    }
    if (bits != 0u) {
        ++xfer_.trim_batches;
        if ((bits & 4u) != 0u) {
            ++xfer_.trim_m64;
        }
    }
}

// Enqueues the host->device copy of `batch`'s run_cpi inputs into the idle device
// input set on the copy queue. No-op without the copy queue, while a prefetch is
// pending, or for an empty batch. The next run_cpi on the same batch object (same
// vectors, same contents) uses those buffers and waits on the copy event instead
// of uploading. The copy reads a staging slot or a pack-pool block, which stay
// busy until it completes.
//
// The caller guarantees that every batch submitted before the newest in-flight
// one has completed (its readback event was waited on), so no device command
// still reads the input set this copy overwrites. The copy then has no
// cross-queue wait on the `alt_free` marker: the host waits on the marker
// first, which returns at once because all its predecessors have completed.
void SurvivorRunner::prefetch_inputs(const SurvivorBatch& batch) {
    if (xfer_.copy_q == nullptr || xfer_.pf_pending || batch.size() == 0) {
        return;
    }
    const std::size_t n = batch.size();
    ensure_alt_inputs(n);
    cl_uint bits = trim_bits_for(batch);
    const std::uint64_t seq = xfer_.pf_seq++;
    const bool skip =
        xfer_.inject_copy && seq >= sc_inject_first_ && seq - sc_inject_first_ < sc_inject_count_;
    // Ordering. WAR: the copy overwrites `alt`, whose readers are the commands
    // enqueued on queue_ before the swap that retired it (mask, input-check
    // reads, tail and slot readbacks of the batch that last ran on it).
    // `alt_free`, a marker enqueued on queue_ at that swap, completes after all
    // of them. The caller issues the copy only after the batch that last read
    // `alt` has completed (verify_gpu's infl_finish_front waits on its readback
    // event), so on the in-order queue_ the marker has nothing left to wait
    // for. It is host-waited anyway, which returns at once (pf_early_wait counts
    // the waits that were not already complete), and the copy goes out with an
    // empty wait list, overlapping the newest batch's mask and tail.
    // RAW: the consuming batch's mask waits on the copy event `ev`
    // (cpi_stage_inputs -> pf_wait).
    // Host memory: the copy reads a staging slot or a pack-pool block, which
    // stays busy until the copy completes.
    cl_event wait = xfer_.alt_free;
    if (wait != nullptr) {
        cl_int es = CL_SUCCESS;
        const cl_int q =
            clGetEventInfo(wait, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(es), &es, nullptr);
        if (q != CL_SUCCESS || es != CL_COMPLETE) {
            ++pipe_.pf_early_wait;
            check(clWaitForEvents(1, &wait), "xfer wait(alt_free)");
        }
        clReleaseEvent(xfer_.alt_free);
        xfer_.alt_free = nullptr;
    }
    ++pipe_.pf_early_n;
    cl_event ev = nullptr;
    upload_inputs(xfer_.copy_q, xfer_.alt, batch, bits, &ev, skip);
    // Required before another queue waits on `ev` (OpenCL 1.2 5.13: a command
    // waited on across queues must have been flushed).
    check(clFlush(xfer_.copy_q), "xfer clFlush(copy)");
    xfer_.pf_pending = true;
    xfer_.pf_ptr = batch.n0.data();
    xfer_.pf_n = n;
    xfer_.pf_first = batch.n0[0];
    xfer_.pf_last = batch.n0[n - 1];
    xfer_.pf_bits = bits;
    xfer_.pf_ev = ev;
    ++xfer_.pf_issued;
}

// Called before releasing host memory that a pending prefetch may read.
void SurvivorRunner::drain_transfers() noexcept {
    if (xfer_.copy_q != nullptr) {
        clFinish(xfer_.copy_q);
    }
    if (queue_ != nullptr) {
        clFinish(queue_);
    }
}

bool SurvivorRunner::prefetch_matches(const SurvivorBatch& batch) const noexcept {
    // Same vectors (data pointer, size) and the same first/last n0: every Step-1
    // survivor appears in exactly one batch, so a recycled vector holding a
    // different batch cannot match.
    const std::size_t n = batch.size();
    return xfer_.pf_pending && n != 0 && xfer_.pf_ptr == batch.n0.data() && xfer_.pf_n == n &&
           std::memcmp(&xfer_.pf_first, &batch.n0[0], sizeof(cl_uint4)) == 0 &&
           std::memcmp(&xfer_.pf_last, &batch.n0[n - 1], sizeof(cl_uint4)) == 0;
}

// Called when `batch` is finished (before its vectors are freed or reused): drops
// a still-pending prefetch of it after waiting for its copy.
void SurvivorRunner::prefetch_forget(const SurvivorBatch& batch) {
    if (!prefetch_matches(batch)) {
        return;
    }
    // The batch finished without consuming its prefetch (e.g. run_cpi's
    // no-candidate early return). Wait for the copy -- it may read the batch's
    // vectors, which the caller is about to free or recycle -- then drop it.
    if (xfer_.pf_ev != nullptr) {
        check(clWaitForEvents(1, &xfer_.pf_ev), "xfer wait(prefetch)");
        clReleaseEvent(xfer_.pf_ev);
        xfer_.pf_ev = nullptr;
    }
    xfer_.pf_pending = false;
}

} // namespace collatz::gpu
