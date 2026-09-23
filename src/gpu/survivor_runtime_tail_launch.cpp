#include "survivor_runtime/internal.hpp"

namespace collatz::gpu {

using namespace survivor_detail;

namespace {

// tail_persistent's flag threshold, computed on the host so the kernel's
// contender test is one compare per candidate (u128q_flag_threshold16).
void bind_flag_thr(cl_kernel k, cl_uint& arg, collatz::u128 rlb) {
    const u128q t =
        u128q_flag_threshold16(u128q_from_halves(collatz::lo64(rlb), collatz::hi64(rlb)));
    cl_uint4 t4;
    for (int i = 0; i < 4; ++i) {
        t4.s[i] = t.w[i];
    }
    check(clSetKernelArg(k, arg++, sizeof(cl_uint4), &t4), "tail arg flag_thr");
}

} // namespace

// Resets the tail's control words, binds every tail_persistent argument and
// launches it on queue_ (shared by cpi_pass2_persistent and cpi_submit). The
// tail reads the candidate count on the device, clamped to `dev_cap`, the
// candidate buffer's capacity. `skip_launch` skips the launch (the INJECT_LOST
// test hook).
void SurvivorRunner::persist_enqueue(const SurvivorConst& c,
                                     collatz::u128 rlb,
                                     std::uint32_t flag_cap,
                                     std::size_t wg,
                                     std::size_t groups,
                                     const PersistBufs& b,
                                     cl_uint nonce,
                                     cl_uint dev_cap,
                                     bool skip_launch) {
    // Reset the flag counter / overflow flag and the global work cursor.
    check(clEnqueueWriteBuffer(
              queue_, b.fc, CL_FALSE, 0, sizeof(cl_uint), &kZeroWord, 0, nullptr, nullptr),
          "persist write(flagcount)");
    check(clEnqueueWriteBuffer(
              queue_, b.fo, CL_FALSE, 0, sizeof(cl_uint), &kZeroWord, 0, nullptr, nullptr),
          "persist write(flagovf)");
    check(clEnqueueWriteBuffer(
              queue_, b.wc, CL_FALSE, 0, sizeof(cl_uint), &kZeroWord, 0, nullptr, nullptr),
          "persist write(workcnt)");

    {
        const cl_mem cand_src = cpi_cand_buf_;
        cl_uint4 preflimit = c.preflimit;
        const cl_uint twoA_mask = c.twoA - 1u;
        const cl_uint cap = flag_cap;
        const auto chunk = static_cast<cl_uint>(persist_chunk_);
        const cl_kernel kp = kern_persistent_;
        const cl_mem tblbuf = tbl16_buf_;
        cl_uint arg = 0;
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &tblbuf), "p arg tbl");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &cand_src), "p arg cand");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &n0_buf_), "p arg n0");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &m_buf_), "p arg m");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &c.a_bits), "p arg a_bits");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &twoA_mask), "p arg twoAm");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &c.m_shift), "p arg mshift");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint4), &preflimit), "p arg pref");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &cap), "p arg cap");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.gcov), "p arg gcov");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.gblk), "p arg gblk");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.gcnt), "p arg gcnt");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.gmax), "p arg gmax");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.fc), "p arg fc");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.flaglist), "p arg fl");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.fo), "p arg fo");
        check(clSetKernelArg(kp, arg++, wg * sizeof(cl_ulong), nullptr), "p loccov");
        check(clSetKernelArg(kp, arg++, wg * sizeof(cl_ulong), nullptr), "p locblk");
        check(clSetKernelArg(kp, arg++, wg * sizeof(cl_uint), nullptr), "p loccnt");
        check(clSetKernelArg(kp, arg++, wg * 4u * sizeof(cl_uint), nullptr), "p locmax");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.gdig), "p arg gdig");
        check(clSetKernelArg(kp, arg++, wg * sizeof(cl_ulong), nullptr), "p locdig");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &chunk), "p arg chunk");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.wc), "p arg workcnt");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &nonce), "p arg nonce");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &b.gchk), "p arg gchk");
        // Device-side candidate count.
        cl_mem dcount = b.dcount != nullptr ? b.dcount : cpi_candcnt_buf_;
        cl_mem dovf = b.dovf != nullptr ? b.dovf : cpi_candovf_buf_;
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &dcount), "p arg dcount");
        check(clSetKernelArg(kp, arg++, sizeof(cl_mem), &dovf), "p arg dovf");
        check(clSetKernelArg(kp, arg++, sizeof(cl_uint), &dev_cap), "p arg dcap");
        bind_trim_args(kp, arg, xfer_.cur_bits, false);
        bind_flag_thr(kp, arg, rlb);
        const std::size_t global = groups * wg;
        const std::size_t local = wg;
        // No fence here; the pass-2 wall is the host time from this enqueue
        // to the final blocking read, which also absorbs any unfenced upload
        // or mask still queued ahead of the kernel.
        if (!skip_launch) {
            if (cl_fail_at_ != 0 && ++tail_launches_ == cl_fail_at_) {
                check(CL_OUT_OF_RESOURCES, "persist NDRange (COLLATZ_GPU_INJECT_CL_FAIL)");
            }
            check(clEnqueueNDRangeKernel(
                      queue_, kp, 1, nullptr, &global, &local, 0, nullptr, nullptr),
                  "persist NDRange");
        }
    }
}

// The fixed persistent-tail grid: persist_mult_ work-groups per compute unit,
// at least one.
std::size_t SurvivorRunner::persist_groups() const noexcept {
    std::size_t cu = info_.max_compute_units;
    if (cu == 0) {
        cu = 1;
    }
    std::size_t groups = persist_mult_ * cu;
    if (groups == 0) {
        groups = 1;
    }
    return groups;
}

} // namespace collatz::gpu
