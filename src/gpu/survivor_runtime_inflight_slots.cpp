#include "survivor_runtime/internal.hpp"

#include <cstdio>

namespace collatz::gpu {

using namespace survivor_detail;

// Two-deep run_cpi pipeline.
namespace {
// Slot layout: the control words {candcnt, candovf, sc[0..3], flagcount,
// flagovf} (workcnt is never read), the input-check rows (16 bytes each, so
// every row is 4-byte aligned; see TransferState::chk), then the per-group
// tail partials and records.
[[nodiscard]] std::size_t infl_round(std::size_t x, std::size_t a) noexcept {
    return (x + a - 1) / a * a;
}
void infl_layout_tail(SlotLayout& l, std::size_t groups, std::size_t al) {
    l.gcov = infl_round(l.gcov, al);
    l.gdig = infl_round(l.gcov + groups * sizeof(cl_ulong), al);
    l.gblk = infl_round(l.gdig + groups * sizeof(cl_ulong), al);
    l.gchk = infl_round(l.gblk + groups * sizeof(cl_ulong), al);
    l.gmax = infl_round(l.gchk + groups * 4u * sizeof(cl_ulong), al);
    l.gcnt = infl_round(l.gmax + groups * sizeof(cl_uint4), al);
}
[[nodiscard]] SlotLayout infl_layout(std::size_t groups) noexcept {
    SlotLayout l;
    l.candcnt = 0;
    l.candovf = 4;
    l.sc = 8;
    l.fc = 24;
    l.fo = 28;
    l.chk = 32;
    l.gcov = 128;
    infl_layout_tail(l, groups, 8);
    l.total = l.gcnt + groups * sizeof(cl_uint);
    return l;
}
// Flag-list prefix: appends the prefix region (k uint entries) to the pinned
// host area. The flag list is its own cl_mem, so it gets its own read in
// cpi_submit.
void infl_layout_flags(SlotLayout& l, std::size_t k) noexcept {
    l.flags_n = k;
    l.flags = infl_round(l.total, 16);
    l.total = l.flags + k * sizeof(cl_uint);
}
} // namespace

void SurvivorRunner::infl_ensure(InflightSlot& s, std::size_t groups, std::size_t flag_cap) {
    cl_int status = CL_SUCCESS;
    const auto mk = [&](std::size_t bytes, cl_mem_flags flags, const char* what) -> cl_mem {
        cl_mem buf = clCreateBuffer(context_, flags, bytes == 0 ? 1 : bytes, nullptr, &status);
        check(status, what);
        return buf;
    };
    const auto drop = [](cl_mem& b) {
        if (b != nullptr) {
            clReleaseMemObject(b); // deferred by CL until its last command is done
            b = nullptr;
        }
    };
    if (flag_cap == 0) {
        flag_cap = 1;
    }
    if (s.flaglist == nullptr || flag_cap > s.flag_cap) {
        drop(s.flaglist);
        s.flaglist = mk(flag_cap * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "slot flaglist");
        s.flag_cap = flag_cap;
    }
    if (groups <= s.groups) {
        return;
    }
    // (Re)build every per-group region. The slot is idle here (cpi_submit
    // checks), so no queued command still targets its host area.
    for (cl_mem* b : {&s.candcnt,
                      &s.candovf,
                      &s.sc,
                      &s.fc,
                      &s.fo,
                      &s.wc,
                      &s.gcov,
                      &s.gdig,
                      &s.gblk,
                      &s.gcnt,
                      &s.gmax,
                      &s.gchk}) {
        drop(*b);
    }
    s.lay = infl_layout(groups);
    infl_layout_flags(s.lay, pipe_.flag_k);
    s.candcnt = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "slot candcnt");
    s.candovf = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "slot candovf");
    s.sc = mk(kScWords * sizeof(cl_uint), CL_MEM_READ_WRITE, "slot sc");
    s.fc = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "slot flagcount");
    s.fo = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "slot flagovf");
    s.wc = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "slot workcnt");
    s.gcov = mk(groups * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "slot gcov");
    s.gdig = mk(groups * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "slot gdig");
    s.gblk = mk(groups * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "slot gblk");
    s.gcnt = mk(groups * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "slot gcnt");
    s.gmax = mk(groups * sizeof(cl_uint4), CL_MEM_WRITE_ONLY, "slot gmax");
    s.gchk = mk(groups * 4u * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "slot gchk");
    // Host readback area: pinned (CL_MEM_ALLOC_HOST_PTR, mapped once and kept
    // mapped, like the staging slots) so the slot's non-blocking reads return at
    // once on a discrete device instead of blocking until the copy is done.
    const std::size_t bytes = s.lay.total;
    if (s.host_buf != nullptr) {
        clEnqueueUnmapMemObject(queue_, s.host_buf, s.host, 0, nullptr, nullptr);
        drop(s.host_buf);
    }
    s.host = nullptr;
    cl_int st = CL_SUCCESS;
    cl_mem hb =
        clCreateBuffer(context_, CL_MEM_ALLOC_HOST_PTR | CL_MEM_READ_WRITE, bytes, nullptr, &st);
    void* h = nullptr;
    if (st == CL_SUCCESS) {
        h = clEnqueueMapBuffer(
            queue_, hb, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, bytes, 0, nullptr, nullptr, &st);
        if (st != CL_SUCCESS || h == nullptr) {
            clReleaseMemObject(hb);
            h = nullptr;
        }
    }
    if (h != nullptr) {
        s.host_buf = hb;
        s.host = static_cast<unsigned char*>(h);
        s.heap.clear();
    } else {
        std::fprintf(stderr,
                     "gpu: pinned readback allocation failed (%s); using pageable memory\n",
                     cl_error_string(st).c_str());
        s.heap.assign(bytes, 0);
        s.host = s.heap.data();
    }
    s.groups = groups;
}

void SurvivorRunner::infl_release() noexcept {
    if (queue_ != nullptr) {
        clFinish(queue_);
    }
    if (queue_alt_ != nullptr) {
        clFinish(queue_alt_);
    }
    for (InflightSlot& s : infl_.slot) {
        if (s.ev != nullptr) {
            clReleaseEvent(s.ev);
            s.ev = nullptr;
        }
        if (s.host_buf != nullptr) {
            if (queue_ != nullptr) {
                clEnqueueUnmapMemObject(queue_, s.host_buf, s.host, 0, nullptr, nullptr);
                clFinish(queue_);
            }
            clReleaseMemObject(s.host_buf);
            s.host_buf = nullptr;
        }
        for (cl_mem* b : {&s.candcnt,
                          &s.candovf,
                          &s.sc,
                          &s.fc,
                          &s.fo,
                          &s.wc,
                          &s.gcov,
                          &s.gdig,
                          &s.gblk,
                          &s.gcnt,
                          &s.gmax,
                          &s.gchk,
                          &s.flaglist}) {
            if (*b != nullptr) {
                clReleaseMemObject(*b);
                *b = nullptr;
            }
        }
        s.host = nullptr;
        s.heap.clear();
        s.groups = 0;
        s.flag_cap = 0;
        s.busy = false;
    }
}

} // namespace collatz::gpu
