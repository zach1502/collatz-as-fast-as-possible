#include "survivor_runtime/internal.hpp"

#include <chrono>
#include <vector>

namespace collatz::gpu {

using namespace survivor_detail;

// The persistent-lane tail over the C packed candidate ids in cpi_cand_buf_,
// with the per-survivor n0, m and 3^f in the input buffers: a fixed grid
// (persist_mult_ * compute units work-groups of `wg`) of lanes that loop, each
// claiming `persist_chunk_` candidates at a time from a global cursor until it
// is drained; inside a SIMD group each round of chains lasts as long as its
// longest chain (tail_persistent). Reads back the per-group partials and the
// flag list, and reduces them into `out` (coverage, blocks, processed, max_bs,
// flags, flag_overflow); every fold is commutative. The tail reads the
// candidate count on the device; `dc` carries the mask words validated here.
void SurvivorRunner::cpi_pass2_persistent(std::size_t C,
                                          const SurvivorConst& c,
                                          collatz::u128 rlb,
                                          std::uint32_t flag_cap,
                                          std::size_t wg,
                                          CpiResult& out,
                                          DevCount& dc) {
    cpi_flag_cap_ = flag_cap;

    // Fixed persistent grid: persist_mult_ * compute_units work-groups, so the
    // per-group partial count is `groups`, independent of C.
    const std::size_t groups = persist_groups();
    ensure_cpi_capacity(C, groups);

    PersistBufs pb;
    pb.fc = cpi_flagcount_buf_;
    pb.fo = cpi_flagovf_buf_;
    pb.wc = cpi_workcnt_buf_;
    pb.gcov = cpi_gcov_buf_;
    pb.gdig = cpi_gdig_buf_;
    pb.gblk = cpi_gblk_buf_;
    pb.gcnt = cpi_gcnt_buf_;
    pb.gmax = cpi_gmax_buf_;
    pb.gchk = cpi_gchk_buf_;
    pb.flaglist = cpi_flaglist_buf_;
    // Test hook (COLLATZ_GPU_SELFCHECK_INJECT_LOST=1): the injected dispatch
    // skips its tail launch, so the group records keep an earlier dispatch's
    // nonce.
    const bool inj_lost = sc_inject_now_ && sc_inject_lost_;
    if (inj_lost) {
        sc_inject_now_ = false;
    }
    const auto t_p0 = std::chrono::steady_clock::now();
    persist_enqueue(c,
                    rlb,
                    flag_cap,
                    wg,
                    groups,
                    pb,
                    sc_cur_nonce_,
                    static_cast<cl_uint>(dc.cand_cap),
                    inj_lost);

    std::vector<cl_ulong> gcov(groups);
    std::vector<cl_ulong> gdig(groups);
    std::vector<cl_ulong> gblk(groups);
    std::vector<cl_uint> gcnt(groups);
    std::vector<cl_uint4> gmax(groups);
    cl_uint flag_count = 0;
    cl_uint flag_ovf = 0;
    std::vector<cl_ulong> gchk(groups * 4u);
    // A throw from a later read drains the queue before the vectors above unwind.
    const DrainOnThrow d(queue_);
    const auto rd = [&](cl_mem buf, std::size_t bytes, void* host, const char* what) {
        check(clEnqueueReadBuffer(queue_, buf, CL_FALSE, 0, bytes, host, 0, nullptr, nullptr),
              what);
    };
    // The mask's cursor, overflow and self-check words ride with the tail
    // readbacks; the last read below blocks on the whole chain.
    rd(cpi_candcnt_buf_, sizeof(cl_uint), &dc.cand_total, "dc read(candcnt)");
    rd(cpi_candovf_buf_, sizeof(cl_uint), &dc.cand_ovf, "dc read(candovf)");
    rd(sc_words_buf_, sizeof(dc.scw), dc.scw, "dc read(sc)");
    rd(cpi_gcov_buf_, groups * sizeof(cl_ulong), gcov.data(), "persist read(gcov)");
    rd(cpi_gdig_buf_, groups * sizeof(cl_ulong), gdig.data(), "persist read(gdig)");
    rd(cpi_gblk_buf_, groups * sizeof(cl_ulong), gblk.data(), "persist read(gblk)");
    rd(cpi_gcnt_buf_, groups * sizeof(cl_uint), gcnt.data(), "persist read(gcnt)");
    rd(cpi_gmax_buf_, groups * sizeof(cl_uint4), gmax.data(), "persist read(gmax)");
    rd(cpi_gchk_buf_, groups * 4u * sizeof(cl_ulong), gchk.data(), "persist read(gchk)");
    rd(cpi_flagcount_buf_, sizeof(cl_uint), &flag_count, "persist read(flagcount)");
    check(
        clEnqueueReadBuffer(
            queue_, cpi_flagovf_buf_, CL_TRUE, 0, sizeof(cl_uint), &flag_ovf, 0, nullptr, nullptr),
        "persist read(flagovf)");
    // The last read blocked, so the chain has completed.
    cpi_pass2_wall_ns_ +=
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t_p0).count();
    // Validate the mask words before any fold; the real candidate count then
    // replaces the sizing bound.
    if (sc_inject_now_ && sc_inject_expand_) {
        dc.scw[2] ^= 1u; // test hook: corrupt the deferred mask count word
        sc_inject_now_ = false;
    }
    if (!sc_check_mask(
            dc.nonce, dc.cand_total, dc.cand_ovf, dc.scw, dc.cupper, dc.cand_cap, dc.groups, out)) {
        return;
    }
    C = static_cast<std::size_t>(dc.cand_total);
    out.sum_pc = dc.cand_total;
    if (sc_inject_now_) {
        gcnt[0] ^= 1u;
    }

    reduce_tail_groups(groups, gcov, gdig, gblk, gcnt, gmax, flag_count, flag_ovf, flag_cap, out);
    if (!out.flag_overflow && flag_count > 0) {
        out.flags.resize(flag_count);
        check(clEnqueueReadBuffer(queue_,
                                  cpi_flaglist_buf_,
                                  CL_TRUE,
                                  0,
                                  flag_count * sizeof(cl_uint),
                                  out.flags.data(),
                                  0,
                                  nullptr,
                                  nullptr),
              "persist read(flaglist)");
    }
    sc_check_tail(sc_cur_nonce_, C, groups, gcov, gdig, gblk, gcnt, gmax, gchk, out);
}

} // namespace collatz::gpu
