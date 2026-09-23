#include "survivor_runtime/internal.hpp"

#include <cstring>

namespace collatz::gpu {

using namespace survivor_detail;

namespace {

// __local scratch (in uints) of the mask kernel at local size L: L+1 straddle
// words, then the work-group's candidate count, id sum and base at words L+1,
// 3L+1 and 5L+1.
[[nodiscard]] constexpr std::size_t mask_local_words(std::size_t L) noexcept {
    return 5 * L + 2;
}

} // namespace

// run_cpi's input staging: swaps in the prefetched set, or uploads into the active
// set on queue_. Returns true on a prefetch hit; `bits` gets the trim bits and
// `pf_wait` the copy event the mask must wait on.
bool SurvivorRunner::cpi_stage_inputs(const SurvivorBatch& batch,
                                      cl_uint& bits,
                                      cl_event& pf_wait) {
    const std::size_t n = batch.size();
    const bool pf_hit = prefetch_matches(batch);
    if (pf_hit) {
        // Copy queue: this batch's inputs were copied into the idle set
        // during the previous batch. Make that set active; the retired set
        // becomes `alt`, and the marker below completes only after every
        // command enqueued so far on queue_ (all of its possible readers).
        const InputSet old = active_inputs();
        set_active_inputs(xfer_.alt);
        xfer_.alt = old;
        if (xfer_.alt_free != nullptr) {
            clReleaseEvent(xfer_.alt_free);
            xfer_.alt_free = nullptr;
        }
        check(clEnqueueMarkerWithWaitList(queue_, 0, nullptr, &xfer_.alt_free), "xfer marker");
        pf_wait = xfer_.pf_ev;
        xfer_.pf_ev = nullptr;
        xfer_.pf_pending = false;
        bits = xfer_.pf_bits;
        ++xfer_.pf_hits;
    } else {
        // The per-survivor input buffers (the mask reads n0/m/astar/fidx; the
        // tail reads n0/m/fidx).
        // With a batch still in flight, this upload is enqueued on the same
        // in-order queue_ after that batch's tail, so it cannot overwrite inputs
        // the tail is reading; ensure_capacity's clReleaseMemObject of a
        // too-small set is deferred by OpenCL until its last reader is done.
        ensure_capacity(n);
        bits = trim_bits_for(batch);
        upload_inputs(queue_, active_inputs(), batch, bits, nullptr, false);
    }
    return pf_hit;
}

// Enqueues the input-freshness reads (kInputCheckRows rows) of the active set into
// `dst`, and records the expected bytes in want and len.
void SurvivorRunner::cpi_enqueue_input_checks(const SurvivorBatch& batch,
                                              cl_uint bits,
                                              unsigned char (*dst)[16],
                                              unsigned char (*want)[16],
                                              std::size_t* len) {
    // Input freshness check; the self-check nonce proves the outputs fresh, not
    // the inputs. Reads back n0[0], n0[n-1], m[n-1], astar[n-1] and fidx[n-1],
    // the last element of the last buffer upload_inputs writes, as the mask
    // saw them: the mask enqueued before these reads waited on the copy. The
    // caller compares them once the batch's readbacks have landed. The copy is
    // a chain of writes on one in-order queue, so the first and last arrays
    // bracket it, and the middle probes catch a lost or stale copy of n0 or m.
    const std::size_t n = batch.size();
    const std::size_t w0 = (bits & 2u) != 0u ? sizeof(cl_ulong) : sizeof(cl_uint4);
    const std::size_t wm = (bits & 4u) != 0u ? sizeof(cl_ulong) : sizeof(cl_uint4);
    // fidx is the f0 index the upload packed (f0 - fmin, one byte).
    const auto fidx_last = static_cast<cl_uchar>(batch.f0[n - 1] - xfer_.fmin);
    const struct {
        cl_mem buf;
        std::size_t idx;
        std::size_t w;
        const void* src;
    } rq[kInputCheckRows] = {{n0_buf_, 0, w0, &batch.n0[0]},
                             {n0_buf_, n - 1, w0, &batch.n0[n - 1]},
                             {m_buf_, n - 1, wm, &batch.m[n - 1]},
                             {astar_buf_, n - 1, sizeof(cl_uint), &batch.astar[n - 1]},
                             {xfer_.act_fidx, n - 1, sizeof(cl_uchar), &fidx_last}};
    for (std::size_t k = 0; k < kInputCheckRows; ++k) {
        // Both layouts are little-endian: the u64 form is the first 8 bytes
        // of the uint4 form when the high limbs are zero.
        std::memcpy(want[k], rq[k].src, rq[k].w);
        len[k] = rq[k].w;
        check(clEnqueueReadBuffer(queue_,
                                  rq[k].buf,
                                  CL_FALSE,
                                  rq[k].idx * rq[k].w,
                                  rq[k].w,
                                  dst[k],
                                  0,
                                  nullptr,
                                  nullptr),
              "xfer read(input check)");
    }
}

// run_cpi's control-word resets and fused mask enqueue. The mask writes the
// candidate list to cpi_cand_buf_ and its cursor, overflow and self-check words
// to cnt, ovf and sc. Waits on `pf_wait` when it is non-null.
void SurvivorRunner::cpi_enqueue_mask(const SurvivorBatch& batch,
                                      const SurvivorConst& c,
                                      cl_uint nonce,
                                      std::size_t cand_cap,
                                      cl_uint bits,
                                      cl_event pf_wait,
                                      cl_mem cnt,
                                      cl_mem ovf,
                                      cl_mem sc) {
    const std::size_t n = batch.size();
    const auto count = static_cast<cl_uint>(n);

    // Reset the candidate write cursor + overflow flag before the fused kernel.
    {
        check(clEnqueueWriteBuffer(
                  queue_, cnt, CL_FALSE, 0, sizeof(cl_uint), &kZeroWord, 0, nullptr, nullptr),
              "cpi write(candcnt)");
        check(clEnqueueWriteBuffer(
                  queue_, ovf, CL_FALSE, 0, sizeof(cl_uint), &kZeroWord, 0, nullptr, nullptr),
              "cpi write(candovf)");
        check(clEnqueueWriteBuffer(
                  queue_, sc, CL_FALSE, 0, sizeof(kZeroWords4), kZeroWords4, 0, nullptr, nullptr),
              "cpi write(sc)");
    }

    arm_dropwg_hook(queue_, sc);

    // The mask adds the survivors its work-group covered into sc word 3, once
    // per group, and the host checks that against the survivor count; a
    // dropped work-group would otherwise shrink the candidate total and the id
    // sum consistently. It runs one work-group per survivor (local size
    // 2^A/32).
    const cl_kernel msk = kern_mask_;
    const auto cap_arg = static_cast<cl_uint>(cand_cap);
    cl_uint arg = 0;
    bind_bv_first(msk, arg, "ms arg bv");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &n0_buf_), "ms arg n0");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &m_buf_), "ms arg m");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &astar_buf_), "ms arg astar");
    check(clSetKernelArg(msk, arg++, sizeof(cl_uint), &count), "ms arg count");
    check(clSetKernelArg(msk, arg++, sizeof(cl_uint), &c.b_mask), "ms arg b_mask");
    check(clSetKernelArg(msk, arg++, sizeof(cl_uint), &c.pow2m_mod9), "ms arg pow2m");
    check(clSetKernelArg(msk, arg++, sizeof(cl_uint), &c.a_bits), "ms arg a_bits");
    check(clSetKernelArg(msk, arg++, sizeof(cl_uint), &cap_arg), "ms arg cap");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &cpi_cand_buf_), "ms arg cand");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &cnt), "ms arg cnt");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &ovf), "ms arg ovf");
    // Local size = 2^A/32 window words, plus 5L+2 uints of __local scratch.
    const std::size_t local = static_cast<std::size_t>(c.twoA >> 5);
    check(clSetKernelArg(msk, arg++, mask_local_words(local) * sizeof(cl_uint), nullptr),
          "ms arg local");
    bind_bv_rest(msk, arg, "ms arg bv-rest");
    check(clSetKernelArg(msk, arg++, sizeof(cl_uint), &nonce), "ms arg nonce");
    check(clSetKernelArg(msk, arg++, sizeof(cl_mem), &sc), "ms arg sc");
    bind_trim_args(msk, arg, bits, true);
    const std::size_t global = n * local;
    check(clEnqueueNDRangeKernel(queue_,
                                 msk,
                                 1,
                                 nullptr,
                                 &global,
                                 &local,
                                 pf_wait != nullptr ? 1u : 0u,
                                 pf_wait != nullptr ? &pf_wait : nullptr,
                                 nullptr),
          "cpi mask_scatter_coalesced NDRange");
}

} // namespace collatz::gpu
