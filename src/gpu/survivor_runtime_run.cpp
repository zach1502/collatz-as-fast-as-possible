#include "survivor_runtime/internal.hpp"

#include <vector>

namespace collatz::gpu {

using namespace survivor_detail;

// Runs one batch through survivor_tail16, one work-item per survivor: the
// fallback for a batch whose flag list overflows. `out` is resized to
// batch.size(). Throws OpenCLError on device failure. Adds kernel and transfer
// time from CL events.
void SurvivorRunner::run(const SurvivorBatch& batch, const SurvivorConst& c, SurvivorOut& out) {
    const std::size_t n = batch.size();
    out.resize(n);
    if (n == 0) {
        return;
    }
    const cl_uint nonce = sc_begin();

    // Buffers persist across batches and grow only when a batch does. The first
    // batch is usually the largest (kFlushCap), so the steady state creates and
    // releases no buffers.
    ensure_capacity(n);
    const DrainOnThrow drain(queue_); // the writes read batch.* until the final read

    // The writes are non-blocking on the in-order queue; the kernel enqueued
    // next waits for them, so no per-write host wait is needed. Their events are
    // kept only to read profiling times after the final blocking read.
    std::vector<cl_event> xfer_events;
    xfer_events.reserve(13);
    const auto write = [&](cl_mem buf, std::size_t bytes, const void* host, const char* what) {
        cl_event ev = nullptr;
        check(clEnqueueWriteBuffer(queue_, buf, CL_FALSE, 0, bytes, host, 0, nullptr, &ev), what);
        xfer_events.push_back(ev);
    };
    write(n0_buf_, n * sizeof(cl_uint4), batch.n0.data(), "write(n0)");
    write(m_buf_, n * sizeof(cl_uint4), batch.m.data(), "write(m)");
    write(astar_buf_, n * sizeof(cl_uint), batch.astar.data(), "write(astar)");
    write(inv3f_buf_, n * sizeof(cl_uint), batch.inv3f.data(), "write(inv3f)");
    write(bvbase_buf_, n * sizeof(cl_uint), batch.bvbase.data(), "write(bvbase)");
    write(bvmode_buf_, n * sizeof(cl_uint), batch.bvmode.data(), "write(bvmode)");
    write(p3f_buf_, n * sizeof(cl_uint4), batch.p3f.data(), "write(p3f)");

    const auto count = static_cast<cl_uint>(n);
    cl_uint4 preflimit = c.preflimit;
    cl_uint arg = 0;
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &tbl16_buf_), "arg tbl16");
    bind_bv_first(kernel_, arg, "arg bv");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &n0_buf_), "arg n0");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &m_buf_), "arg m");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &astar_buf_), "arg astar");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &inv3f_buf_), "arg inv3f");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &bvbase_buf_), "arg bvbase");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &bvmode_buf_), "arg bvmode");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &p3f_buf_), "arg p3f");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint), &count), "arg count");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint), &c.b_mask), "arg b_mask");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint), &c.twoA), "arg twoA");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint), &c.m_shift), "arg m_shift");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint), &c.pow2m_mod9), "arg pow2m_mod9");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint4), &preflimit), "arg preflimit");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &cov_buf_), "arg cov");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &blocks_buf_), "arg blocks");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &cand_buf_), "arg cand");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &maxv_buf_), "arg maxv");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &ovf_buf_), "arg ovf");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &pc_buf_), "arg pc");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &dig_buf_), "arg dig");
    bind_bv_rest(kernel_, arg, "arg bv-rest");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_uint), &nonce), "arg nonce");
    check(clSetKernelArg(kernel_, arg++, sizeof(cl_mem), &chk_buf_), "arg chk");

    const std::size_t global = n;
    cl_event kev = nullptr;
    const std::size_t n_writes = xfer_events.size();
    check(clEnqueueNDRangeKernel(queue_, kernel_, 1, nullptr, &global, nullptr, 0, nullptr, &kev),
          "clEnqueueNDRangeKernel");

    // Device->host readbacks, non-blocking except the last (which flushes the
    // whole in-order chain), so a dispatch has one host wait.
    const auto read =
        [&](cl_mem buf, std::size_t bytes, void* host, bool blocking, const char* what) {
            cl_event ev = nullptr;
            const cl_bool bl = blocking ? CL_TRUE : CL_FALSE;
            check(clEnqueueReadBuffer(queue_, buf, bl, 0, bytes, host, 0, nullptr, &ev), what);
            xfer_events.push_back(ev);
        };
    read(cov_buf_, n * sizeof(cl_ulong), out.cov.data(), false, "read(cov)");
    read(dig_buf_, n * sizeof(cl_ulong), out.dig.data(), false, "read(dig)");
    read(blocks_buf_, n * sizeof(cl_ulong), out.blocks.data(), false, "read(blocks)");
    read(cand_buf_, n * sizeof(cl_uint), out.cand.data(), false, "read(cand)");
    read(maxv_buf_, n * sizeof(cl_uint4), out.maxv.data(), false, "read(maxv)");
    read(ovf_buf_, n * sizeof(cl_uint), out.ovf.data(), false, "read(ovf)");
    read(chk_buf_, n * sizeof(cl_ulong), out.chk.data(), false, "read(chk)");
    read(pc_buf_, n * sizeof(cl_uint), out.pc.data(), true, "read(pc)");
    // There are no fences, so the three phase walls come from the completed
    // events: write span, kernel, readback span.
    add_span_ns(xfer_events.front(), xfer_events[n_writes - 1], write_wall_ns_);
    add_event_ns(kev, kernel_wall_ns_);
    add_span_ns(xfer_events[n_writes], xfer_events.back(), read_wall_ns_);

    if (sc_inject_now_) {
        out.cov[0] ^= 1u;
    }
    for (std::size_t j = 0; j < n && out.sc_ok; ++j) {
        if (out.chk[j] != u128q_sc_item(nonce,
                                        static_cast<cl_uint>(j),
                                        out.cov[j],
                                        out.dig[j],
                                        out.blocks[j],
                                        out.cand[j],
                                        sc_u128q(out.maxv[j]),
                                        out.ovf[j],
                                        out.pc[j])) {
            out.sc_ok = false;
            out.sc_why = "item record hash";
        } else if (out.cand[j] > out.pc[j] || out.pc[j] > c.twoA) {
            out.sc_ok = false;
            out.sc_why = "item counts";
        }
    }

    // The final read was blocking, so every event has completed.
    add_event_ns(kev, kernel_ns_);
    clReleaseEvent(kev);
    for (cl_event ev : xfer_events) {
        add_event_ns(ev, transfer_ns_);
        clReleaseEvent(ev);
    }
}

} // namespace collatz::gpu
