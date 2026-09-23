#include "host_pipeline.hpp"
#include "survivor_runtime/internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace collatz::gpu {

using namespace survivor_detail;

// Arms (or disarms) the dropped-work-group hook in sc word 4 of `sc` for the
// current dispatch; consumes sc_inject_now_ when it fires.
void SurvivorRunner::arm_dropwg_hook(cl_command_queue q, cl_mem sc) {
    if (!sc_inject_dropwg_) {
        return; // the kernels never read word 4 without the test-hook define
    }
    const bool drop = sc_inject_now_;
    sc_inject_now_ = false; // the dropped group is this dispatch's only fault
    check(clEnqueueWriteBuffer(q,
                               sc,
                               CL_FALSE,
                               4u * sizeof(cl_uint),
                               sizeof(cl_uint),
                               drop ? &kOneWord : &kZeroWord,
                               0,
                               nullptr,
                               nullptr),
          "mask write(sc drop hook)");
}

// Draws the next dispatch nonce and decides whether the injection hook corrupts
// this dispatch's readback.
cl_uint SurvivorRunner::sc_begin() noexcept {
    ++sc_nonce_;
    if (sc_nonce_ == 0) {
        sc_nonce_ = 1;
    }
    sc_inject_now_ = sc_seq_ >= sc_inject_first_ && sc_seq_ - sc_inject_first_ < sc_inject_count_;
    if (xfer_.inject_copy) {
        sc_inject_now_ = false; // the fault is a lost prefetch copy instead
    }
    ++sc_seq_;
    sc_cur_nonce_ = sc_nonce_;
    return sc_nonce_;
}

// Folds one tail's verified records into `out` and checks the batch count and flag
// invariants (claimed == C, flagged == flag_count, flag-list sum).
void SurvivorRunner::sc_check_tail(cl_uint nonce,
                                   std::size_t C,
                                   std::size_t groups,
                                   const std::vector<cl_ulong>& gcov,
                                   const std::vector<cl_ulong>& gdig,
                                   const std::vector<cl_ulong>& gblk,
                                   const std::vector<cl_uint>& gcnt,
                                   const std::vector<cl_uint4>& gmax,
                                   const std::vector<cl_ulong>& gchk,
                                   CpiResult& out) const {
    out.sc_checked = true;
    std::uint64_t claimed = 0;
    std::uint64_t nfl = 0;
    std::uint32_t ids = 0;
    std::uint32_t fls = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        const cl_ulong* r = &gchk[4 * g];
        if (r[3] != nonce) {
            sc_fail(out, "tail group nonce");
            return;
        }
        if (r[0] != u128q_sc_group(
                        nonce, gcov[g], gdig[g], gblk[g], gcnt[g], sc_u128q(gmax[g]), r[1], r[2])) {
            sc_fail(out, "tail group record hash");
            return;
        }
        claimed += r[1] >> 32;
        nfl += r[1] & 0xFFFFFFFFu;
        ids += static_cast<std::uint32_t>(r[2] & 0xFFFFFFFFu);
        fls += static_cast<std::uint32_t>(r[2] >> 32);
    }
    if (claimed != C) {
        sc_fail(out, "tail claimed != candidate count");
    }
    if (out.processed > claimed) {
        sc_fail(out, "tail processed > claimed");
    }
    if (nfl != out.flag_count) {
        sc_fail(out, "tail flagged != flag counter");
    }
    if (!out.flag_overflow) {
        std::uint32_t sum = 0;
        for (const std::uint32_t f : out.flags) {
            sum += u128q_sc_term(f, nonce);
        }
        if (sum != fls) {
            sc_fail(out, "flag list sum");
        }
    }
    out.sc_tail_ids = ids;
}

// Device-mask candidate-count and self-check validation, shared by the eager
// run_cpi path and the device-count path. Returns true when the batch may go on to
// (or keep) its tail result; on false, `out` carries either sc_fail or
// flag_overflow (the per-survivor fallback).
bool SurvivorRunner::sc_check_mask(cl_uint nonce,
                                   cl_uint cand_total,
                                   cl_uint cand_ovf,
                                   const cl_uint* scw,
                                   std::uint64_t cupper,
                                   std::size_t cand_cap,
                                   cl_uint expect_groups,
                                   CpiResult& out) {
    if (scw[1] != nonce) {
        sc_fail(out, "mask nonce");
        return false;
    }
    if (scw[3] != expect_groups) {
        // A dropped mask work-group lowers the count and the id sum
        // consistently, so only the per-group unit count (word 3) sees it.
        ++mask_wg_fail_;
        sc_fail(out, "mask work-group count");
        return false;
    }
    if (scw[2] != cand_total || cand_total > cupper) {
        sc_fail(out, "mask candidate count");
        return false;
    }
    if (cand_ovf != 0u) {
        // Candidate buffer full: the caller splits the batch, or falls back to
        // run() for a single survivor.
        out.flag_overflow = true;
        out.cand_overflow = true;
        return false;
    }
    if (cand_total > cand_cap) {
        sc_fail(out, "mask count exceeds buffer without overflow flag");
        return false;
    }
    return true;
}

// Validates the input-check rows of a consumed prefetch once (xfer_.chk_on).
// Returns true, with `out` failed, when a row differs from what was uploaded.
bool SurvivorRunner::prefetch_inputs_bad(CpiResult& out) {
    if (!xfer_.chk_on) {
        return false;
    }
    xfer_.chk_on = false;
    for (std::size_t k = 0; k < kInputCheckRows; ++k) {
        if (std::memcmp(xfer_.chk[k], xfer_.chk_want[k], xfer_.chk_len[k]) != 0) {
            ++xfer_.chk_fail;
            sc_fail(out, "prefetched input readback");
            return true;
        }
    }
    return false;
}

// The completing half of the two-deep pipeline; cpi_submit describes both.
void SurvivorRunner::cpi_complete(int si, CpiResult& out) {
    out = CpiResult{};
    if (si < 0 || si > 1 || !infl_.slot[si].busy) {
        throw std::logic_error("cpi_complete on an idle slot");
    }
    InflightSlot& s = infl_.slot[si];
    ++sc_seq_; // this batch validates here (see the sequence note in cpi_submit)
    struct Done {
        InflightSlot& s;
        ~Done() {
            s.busy = false;
            if (s.ev != nullptr) {
                clReleaseEvent(s.ev);
                s.ev = nullptr;
            }
        }
    } done{s};
    const auto w0 = std::chrono::steady_clock::now();
    check(clWaitForEvents(1, &s.ev), "slot wait(readback)");
    infl_.wait_ns +=
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - w0).count();
    const std::size_t groups = s.tail_groups;
    const SlotLayout& L = s.lay;
    const unsigned char* const h = s.host;
    const auto word = [h](std::size_t off) {
        cl_uint v = 0;
        std::memcpy(&v, h + off, sizeof(v));
        return v;
    };
    const cl_uint cand_total = word(L.candcnt);
    const cl_uint cand_ovf = word(L.candovf);
    cl_uint scw[4] = {word(L.sc), word(L.sc + 4), word(L.sc + 8), word(L.sc + 12)};
    const cl_uint flag_count = word(L.fc);
    const cl_uint flag_ovf = word(L.fo);
    if (s.chk_on) {
        for (std::size_t k = 0; k < kInputCheckRows; ++k) {
            if (std::memcmp(h + L.chk + 16u * k, s.chk_want[k], s.chk_len[k]) != 0) {
                ++xfer_.chk_fail;
                sc_fail(out, "prefetched input readback");
                return;
            }
        }
    }
    if (s.inject && sc_inject_expand_) {
        scw[2] ^= 1u; // test hook: corrupt the deferred mask count word
        s.inject = false;
    }
    if (!sc_check_mask(
            s.nonce, cand_total, cand_ovf, scw, s.cupper, s.cand_cap, s.mask_groups, out)) {
        return; // sc failure, or a candidate-buffer overflow (caller splits)
    }
    const auto C = static_cast<std::size_t>(cand_total);
    out.sum_pc = cand_total;
    InflightState& f = infl_;
    f.gcov.resize(groups);
    f.gdig.resize(groups);
    f.gblk.resize(groups);
    f.gcnt.resize(groups);
    f.gmax.resize(groups);
    f.gchk.resize(groups * 4u);
    std::memcpy(f.gcov.data(), h + L.gcov, groups * sizeof(cl_ulong));
    std::memcpy(f.gdig.data(), h + L.gdig, groups * sizeof(cl_ulong));
    std::memcpy(f.gblk.data(), h + L.gblk, groups * sizeof(cl_ulong));
    std::memcpy(f.gcnt.data(), h + L.gcnt, groups * sizeof(cl_uint));
    std::memcpy(f.gmax.data(), h + L.gmax, groups * sizeof(cl_uint4));
    std::memcpy(f.gchk.data(), h + L.gchk, groups * 4u * sizeof(cl_ulong));
    if (s.inject) {
        f.gcnt[0] ^= 1u; // test hook: a stale or short readback
        s.inject = false;
    }
    reduce_tail_groups(
        groups, f.gcov, f.gdig, f.gblk, f.gcnt, f.gmax, flag_count, flag_ovf, s.flag_cap_used, out);
    if (!out.flag_overflow && flag_count > 0) {
        // The flag list is read only now that its length is known. On the side
        // queue (waiting on the slot's event) it does not queue behind the
        // other slot's tail; without a side queue it would, which is correct
        // but serializes the pipeline for flagged batches.
        out.flags.resize(flag_count);
        // Flag-list prefix: entries [0, flag_pref) already landed with the slot
        // readback (s.ev was waited on above); only the rest needs a device read.
        const std::size_t pref = std::min<std::size_t>(s.flag_pref, flag_count);
        std::memcpy(out.flags.data(), h + L.flags, pref * sizeof(cl_uint));
        const std::size_t rest = flag_rest_len(pref, flag_count);
        ++(rest == 0 ? pipe_.flag_prefix_ok : pipe_.flag_rest_rd);
        if (rest != 0) {
            cl_command_queue rq = queue_alt_ != nullptr ? queue_alt_ : queue_;
            check(clEnqueueReadBuffer(rq,
                                      s.flaglist,
                                      CL_TRUE,
                                      pref * sizeof(cl_uint),
                                      rest * sizeof(cl_uint),
                                      out.flags.data() + pref,
                                      1,
                                      &s.ev,
                                      nullptr),
                  "slot read(flaglist)");
        }
    }
    sc_check_tail(s.nonce, C, groups, f.gcov, f.gdig, f.gblk, f.gcnt, f.gmax, f.gchk, out);
    ++dc_batches_;
    if (out.sc_checked && out.sc_ok && out.sc_tail_ids != scw[0]) {
        sc_fail(out, "tail/mask candidate id sum");
    }
}

} // namespace collatz::gpu
