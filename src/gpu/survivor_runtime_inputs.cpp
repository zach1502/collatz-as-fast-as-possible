#include "survivor_runtime/internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace collatz::gpu {

using namespace survivor_detail;

// Dispatch-thread transfer path. A non-blocking transfer's host memory must
// stay valid and unchanged until the command completes:
//  * Uploads read a staging slot or a pack-pool block, never the caller's
//    vectors. A slot's `ev` is the event of the last write that read it;
//    writes of one upload sit on one in-order queue, so `ev` complete implies
//    every write of that upload completed. next_staging() waits on `ev` before
//    refilling the slot, and release_transfer() drains both queues before
//    unmapping/freeing it.
//  * Device input sets: the copy of batch i+1 goes into `alt`, which was last
//    read by commands enqueued on queue_ before the swap that retired it; the
//    copy waits on `alt_free`, a marker enqueued on queue_ at that swap (WAR),
//    and the mask of batch i+1 waits on the copy event (RAW).

SurvivorRunner::InputSet SurvivorRunner::active_inputs() const noexcept {
    InputSet s;
    s.n0 = n0_buf_;
    s.m = m_buf_;
    s.astar = astar_buf_;
    s.inv3f = inv3f_buf_;
    s.bvbase = bvbase_buf_;
    s.bvmode = bvmode_buf_;
    s.p3f = p3f_buf_;
    s.fidx = xfer_.act_fidx;
    s.cap = xfer_.act_cap;
    return s;
}

void SurvivorRunner::set_active_inputs(const InputSet& s) noexcept {
    n0_buf_ = s.n0;
    m_buf_ = s.m;
    astar_buf_ = s.astar;
    inv3f_buf_ = s.inv3f;
    bvbase_buf_ = s.bvbase;
    bvmode_buf_ = s.bvmode;
    p3f_buf_ = s.p3f;
    xfer_.act_fidx = s.fidx;
    xfer_.act_cap = s.cap;
    // run()'s outputs keep their own size; ensure_capacity() re-creates the
    // whole active set (inputs + outputs) whenever either is too small.
    capacity_ = std::min(xfer_.act_cap, xfer_.out_cap);
}

void SurvivorRunner::ensure_alt_inputs(std::size_t n) {
    InputSet& a = xfer_.alt;
    if (a.cap >= n) {
        return;
    }
    // clReleaseMemObject defers the free until commands using the buffer have
    // completed, and no command enqueued after this point references it.
    for (cl_mem* b : {&a.n0, &a.m, &a.astar, &a.inv3f, &a.bvbase, &a.bvmode, &a.p3f, &a.fidx}) {
        if (*b != nullptr) {
            clReleaseMemObject(*b);
            *b = nullptr;
        }
    }
    a.cap = 0;
    cl_int status = CL_SUCCESS;
    const auto mk = [&](std::size_t bytes, const char* what) -> cl_mem {
        cl_mem buf = clCreateBuffer(context_, CL_MEM_READ_ONLY, bytes, nullptr, &status);
        check(status, what);
        return buf;
    };
    a.n0 = mk(n * sizeof(cl_uint4), "clCreateBuffer(alt n0)");
    a.m = mk(n * sizeof(cl_uint4), "clCreateBuffer(alt m)");
    a.astar = mk(n * sizeof(cl_uint), "clCreateBuffer(alt astar)");
    a.inv3f = mk(n * sizeof(cl_uint), "clCreateBuffer(alt inv3f)");
    a.bvbase = mk(n * sizeof(cl_uint), "clCreateBuffer(alt bvbase)");
    a.bvmode = mk(n * sizeof(cl_uint), "clCreateBuffer(alt bvmode)");
    a.p3f = mk(n * sizeof(cl_uint4), "clCreateBuffer(alt p3f)");
    a.fidx = mk(n * sizeof(cl_uchar), "clCreateBuffer(alt fidx)");
    a.cap = n;
}

SurvivorRunner::StagingSlot& SurvivorRunner::next_staging(std::size_t bytes) {
    if (xfer_.prepack.valid && xfer_.prepack.slot == xfer_.next_stage) {
        // The slot is about to be refilled, so its pending pre-pack is lost;
        // that batch is packed again at prefetch time.
        xfer_.prepack.valid = false;
        ++pipe_.prepack_lost;
    }
    StagingSlot& st = xfer_.stage[xfer_.next_stage];
    xfer_.next_stage ^= 1;
    if (st.ev != nullptr) {
        // Reuse rule: the last write that read this slot has completed.
        check(clWaitForEvents(1, &st.ev), "xfer wait(stage)");
        clReleaseEvent(st.ev);
        st.ev = nullptr;
    }
    if (st.bytes >= bytes) {
        return st;
    }
    const std::size_t want = bytes + bytes / 8; // grow-only, with headroom
    if (st.buf != nullptr) {
        // Already idle (ev waited above): unmap, then free (deferred by CL).
        clEnqueueUnmapMemObject(queue_, st.buf, st.host, 0, nullptr, nullptr);
        clReleaseMemObject(st.buf);
        st.buf = nullptr;
        st.host = nullptr;
    }
    st.bytes = 0;
    if (!xfer_.pinned_failed) {
        cl_int status = CL_SUCCESS;
        cl_mem b = clCreateBuffer(
            context_, CL_MEM_ALLOC_HOST_PTR | CL_MEM_READ_WRITE, want, nullptr, &status);
        void* h = nullptr;
        if (status == CL_SUCCESS) {
            // Mapped once and kept mapped: the pinned-transfer idiom. The mapped
            // pointer is ordinary host memory for the write commands below.
            h = clEnqueueMapBuffer(
                queue_, b, CL_TRUE, CL_MAP_WRITE, 0, want, 0, nullptr, nullptr, &status);
            if (status != CL_SUCCESS) {
                clReleaseMemObject(b);
            }
        }
        if (status == CL_SUCCESS && h != nullptr) {
            st.buf = b;
            st.host = static_cast<unsigned char*>(h);
            st.bytes = want;
            st.heap.clear();
            st.heap.shrink_to_fit();
            return st;
        }
        xfer_.pinned_failed = true;
        std::fprintf(stderr,
                     "gpu: pinned staging allocation failed (%s); using pageable staging\n",
                     cl_error_string(status).c_str());
    }
    st.heap.resize(want);
    st.host = st.heap.data();
    st.bytes = want;
    return st;
}

// Trim bits to try for `batch`; upload_inputs may clear bits 1 and 2 when a
// value violates their precondition. Bit 0 (the f index) is always set:
// upload_setup installs the per-f tables before the first batch, for every f
// in [fmin, M], and every survivor in a batch has such an f.
cl_uint SurvivorRunner::trim_bits_for(const SurvivorBatch& batch) const {
    assert(xfer_.ftab != nullptr && xfer_.p3tab != nullptr && batch.f0.size() == batch.size());
    cl_uint bits = 1u; // f table
    if (batch.hint_n0_hi == 0u) {
        bits |= 2u;
    }
    if (batch.hint_m_hi == 0u) {
        bits |= 4u;
    }
    return bits;
}

void SurvivorRunner::bind_trim_args(cl_kernel k, cl_uint& arg, cl_uint bits, bool mask_table) {
    cl_mem fidx = xfer_.act_fidx;
    cl_mem tab = mask_table ? xfer_.ftab : xfer_.p3tab;
    check(clSetKernelArg(k, arg++, sizeof(cl_uint), &bits), "xfer arg trim");
    check(clSetKernelArg(k, arg++, sizeof(cl_mem), &fidx), "xfer arg fidx");
    check(clSetKernelArg(k, arg++, sizeof(cl_mem), &tab), "xfer arg ftab");
}

// Upload-trim tables: for f = fmin + i (i < p3f.size()), p3f[i] = 3^f and
// fcol[i] = {inv3f, bvbase, bvmode, 0}, as build_inputs computes them. Uploaded
// once. f comes from SurvivorBatch::f0, and the kernels read the f-dependent
// inputs only from these tables.
void SurvivorRunner::set_trim_tables(unsigned fmin,
                                     const std::vector<cl_uint4>& p3f,
                                     const std::vector<cl_uint4>& fcol) {
    if (p3f.empty() || p3f.size() != fcol.size() || p3f.size() > 256) {
        return;
    }
    for (cl_mem* b : {&xfer_.ftab, &xfer_.p3tab}) {
        if (*b != nullptr) {
            clReleaseMemObject(*b);
            *b = nullptr;
        }
    }
    cl_int status = CL_SUCCESS;
    xfer_.ftab = clCreateBuffer(context_,
                                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                fcol.size() * sizeof(cl_uint4),
                                const_cast<cl_uint4*>(fcol.data()),
                                &status);
    check(status, "clCreateBuffer(xfer ftab)");
    xfer_.p3tab = clCreateBuffer(context_,
                                 CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                 p3f.size() * sizeof(cl_uint4),
                                 const_cast<cl_uint4*>(p3f.data()),
                                 &status);
    check(status, "clCreateBuffer(xfer p3tab)");
    xfer_.fmin = fmin;
    xfer_.fcount = p3f.size();
}

void SurvivorRunner::release_transfer() noexcept {
    drain_transfers();
    release_pack_pool(); // the queues are drained, so no write reads a block
    for (StagingSlot& st : xfer_.stage) {
        if (st.ev != nullptr) {
            clReleaseEvent(st.ev);
            st.ev = nullptr;
        }
        if (st.buf != nullptr) {
            if (queue_ != nullptr) {
                clEnqueueUnmapMemObject(queue_, st.buf, st.host, 0, nullptr, nullptr);
                clFinish(queue_);
            }
            clReleaseMemObject(st.buf);
            st.buf = nullptr;
        }
        st.host = nullptr;
        st.bytes = 0;
        st.heap.clear();
    }
    for (cl_event* e : {&xfer_.pf_ev, &xfer_.alt_free}) {
        if (*e != nullptr) {
            clReleaseEvent(*e);
            *e = nullptr;
        }
    }
    xfer_.pf_pending = false;
    xfer_.prepack.valid = false;
    InputSet& a = xfer_.alt;
    for (cl_mem* b : {&a.n0,
                      &a.m,
                      &a.astar,
                      &a.inv3f,
                      &a.bvbase,
                      &a.bvmode,
                      &a.p3f,
                      &a.fidx,
                      &xfer_.ftab,
                      &xfer_.p3tab}) {
        if (*b != nullptr) {
            clReleaseMemObject(*b);
            *b = nullptr;
        }
    }
    a.cap = 0;
    if (xfer_.copy_q != nullptr) {
        clReleaseCommandQueue(xfer_.copy_q);
        xfer_.copy_q = nullptr;
    }
}

} // namespace collatz::gpu
