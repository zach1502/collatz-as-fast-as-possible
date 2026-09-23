#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_INTERNAL_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_INTERNAL_HPP

// Private declarations shared by the survivor_runtime*.cpp files: the OpenCL
// status check, self-check and event-timing helpers, scope guards, and the
// private nested types of SurvivorRunner and SurvivorRunnerTransfer. The .cpp
// files use the survivor_detail names unqualified (using namespace
// survivor_detail).

#include "collatz/gpu/u128_limbs.h" // shared mod_mersenne61 for the host fold reduction
#include "opencl_runtime.hpp"       // cl_error_string, OpenCLError
#include "survivor_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace collatz::gpu {

namespace survivor_detail {

// Throws an OpenCLError naming the failed call and its decoded status.
inline void check(cl_int status, const char* what) {
    if (status != CL_SUCCESS) {
        throw OpenCLError(std::string(what) + " failed: " + cl_error_string(status));
    }
}

// Sources for the non-blocking counter resets; they must outlive the enqueued
// writes.
inline constexpr cl_uint kZeroWord = 0;
inline constexpr cl_uint kZeroWords4[4] = {0, 0, 0, 0}; // self-check word reset
inline constexpr cl_uint kOneWord = 1;                  // arms the dropped-group hook
// Self-check words: {id sum, nonce, candidate count, unit count}, then word 4,
// the dropped-work-group test hook (read only by a -DCOLLATZ_GPU_INJECT_DROPWG
// build).
inline constexpr std::size_t kScWords = 8;

[[nodiscard]] inline u128q sc_u128q(const cl_uint4& v) {
    u128q q;
    for (int i = 0; i < 4; ++i) {
        q.w[i] = v.s[i];
    }
    return q;
}

inline void sc_fail(CpiResult& out, const char* why) {
    if (out.sc_ok) {
        out.sc_ok = false;
        out.sc_why = why;
    }
}

// Alignment in bytes of each region inside a staging slot.
inline constexpr std::size_t kStagingAlign = 256;
[[nodiscard]] constexpr std::size_t staging_round_up(std::size_t x) noexcept {
    return (x + kStagingAlign - 1) / kStagingAlign * kStagingAlign;
}

// Adds the span from the START of `first` to the END of `last` (completed
// commands on one in-order queue) to `acc_ns`. run() takes its phase walls
// from it, since it has no fences.
inline void add_span_ns(cl_event first, cl_event last, double& acc_ns) {
    if (first == nullptr || last == nullptr) {
        return;
    }
    cl_ulong start = 0;
    cl_ulong end = 0;
    if (clGetEventProfilingInfo(
            first, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr) == CL_SUCCESS &&
        clGetEventProfilingInfo(last, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr) ==
            CL_SUCCESS &&
        end >= start) {
        acc_ns += static_cast<double>(end - start);
    }
}

// When the enclosing scope exits by a throw, drains the queue first, so
// non-blocking transfers that still reference caller-owned host memory finish
// before that memory unwinds. No clFinish sits between the uploads and the
// final blocking read.
class DrainOnThrow {
public:
    explicit DrainOnThrow(cl_command_queue q) noexcept : q_(q), n_(std::uncaught_exceptions()) {}
    DrainOnThrow(const DrainOnThrow&) = delete;
    DrainOnThrow& operator=(const DrainOnThrow&) = delete;
    ~DrainOnThrow() {
        if (std::uncaught_exceptions() > n_) {
            clFinish(q_);
        }
    }

private:
    cl_command_queue q_;
    int n_;
};

// DrainOnThrow for both the compute queue and the copy queue.
class DrainBothOnThrow {
public:
    DrainBothOnThrow(cl_command_queue a, cl_command_queue b) noexcept
        : a_(a), b_(b), n_(std::uncaught_exceptions()) {}
    DrainBothOnThrow(const DrainBothOnThrow&) = delete;
    DrainBothOnThrow& operator=(const DrainBothOnThrow&) = delete;
    ~DrainBothOnThrow() {
        if (std::uncaught_exceptions() > n_) {
            if (b_ != nullptr) {
                clFinish(b_);
            }
            if (a_ != nullptr) {
                clFinish(a_);
            }
        }
    }

private:
    cl_command_queue a_;
    cl_command_queue b_;
    int n_;
};

// Adds a completed event's [START, END) nanoseconds to `acc_ns`.
inline void add_event_ns(cl_event ev, double& acc_ns) {
    if (ev == nullptr) {
        return;
    }
    cl_ulong start = 0;
    cl_ulong end = 0;
    if (clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr) ==
            CL_SUCCESS &&
        clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr) ==
            CL_SUCCESS &&
        end >= start) {
        acc_ns += static_cast<double>(end - start);
    }
}

// Reduces the per-work-group partials of one tail launch into `out`: coverage
// and digest mod 2^61-1, blocks and counts wrapping, the largest block-start
// value, and the flag count, which overflows past `flag_cap`.
inline void reduce_tail_groups(std::size_t groups,
                               const std::vector<cl_ulong>& gcov,
                               const std::vector<cl_ulong>& gdig,
                               const std::vector<cl_ulong>& gblk,
                               const std::vector<cl_uint>& gcnt,
                               const std::vector<cl_uint4>& gmax,
                               cl_uint flag_count,
                               cl_uint flag_ovf,
                               std::uint32_t flag_cap,
                               CpiResult& out) {
    std::uint64_t cov_acc = 0;
    std::uint64_t dig_acc = 0;
    std::uint64_t blk_acc = 0;
    std::uint64_t cnt_acc = 0;
    collatz::u128 max_bs = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        cov_acc = static_cast<std::uint64_t>(u128q_mod_mersenne61(cov_acc + gcov[g]));
        dig_acc = static_cast<std::uint64_t>(u128q_mod_mersenne61(dig_acc + gdig[g]));
        blk_acc += gblk[g];
        cnt_acc += gcnt[g];
        const std::uint64_t lo = static_cast<std::uint64_t>(gmax[g].s[0]) |
                                 (static_cast<std::uint64_t>(gmax[g].s[1]) << 32);
        const std::uint64_t hi = static_cast<std::uint64_t>(gmax[g].s[2]) |
                                 (static_cast<std::uint64_t>(gmax[g].s[3]) << 32);
        const collatz::u128 v = collatz::make_u128(hi, lo);
        if (v > max_bs) {
            max_bs = v;
        }
    }
    out.coverage = cov_acc;
    out.digest = dig_acc;
    out.blocks = blk_acc;
    out.processed = cnt_acc;
    out.max_bs = max_bs;
    out.flag_count = flag_count;
    out.flag_overflow = (flag_ovf != 0u) || (flag_count > flag_cap);
}

// Releases the event it holds, if any, when the scope exits.
struct EventGuard {
    cl_event e = nullptr;
    EventGuard() = default;
    EventGuard(const EventGuard&) = delete;
    EventGuard& operator=(const EventGuard&) = delete;
    ~EventGuard() {
        if (e != nullptr) {
            clReleaseEvent(e);
        }
    }
};

} // namespace survivor_detail

/// Device-mask context for cpi_pass2_persistent, whose tail reads the
/// candidate count on the device. The mask's cursor, overflow and self-check
/// words are read back with the tail's outputs and validated (sc_check_mask)
/// before any fold.
struct SurvivorRunner::DevCount {
    cl_uint nonce = 0;
    std::uint64_t cupper = 0; // host upper bound sum of min(a*, 2^A)
    std::size_t cand_cap = 0; // candidate buffer capacity
    cl_uint cand_total = 0;   // readback targets
    cl_uint cand_ovf = 0;
    cl_uint scw[4] = {0, 0, 0, 0};
    cl_uint groups = 0; // expected mask survivors in scw[3] (always checked)
};

/// The buffers one tail_persistent launch writes: the serial cpi_*_buf_ set
/// or an in-flight slot.
struct SurvivorRunner::PersistBufs {
    cl_mem fc = nullptr, fo = nullptr, wc = nullptr, gcov = nullptr, gdig = nullptr, gblk = nullptr,
           gcnt = nullptr, gmax = nullptr, gchk = nullptr, flaglist = nullptr;
    cl_mem dcount = nullptr, dovf = nullptr; // device-count words
};

struct SurvivorRunner::PackedBlock {
    int block = -1;
    int nw = 0;
    cl_uint bits = 0;
};

// Producer-side input packing. Without it the staged upload packs each batch's
// narrowed arrays into a staging slot on the single dispatch thread. With it
// the producer threads pack each batch into a block of a fixed pool (pinned
// unless pinning failed) before pushing it, and upload_inputs enqueues the
// writes straight from that block, with no copy. A block goes back to a
// producer only after the last write that read it has completed (polled on the
// dispatch thread). Any mismatch (no free
// block, layout too big, trim bits differ at upload time, a second upload of the
// same batch) falls back to the dispatch-thread pack, so every Result scalar is
// unchanged. Block states change under `mtx`:
//   Free -> Packing (pack_on_producer) -> Packed (the batch holds its index and
//   generation) -> InFlight (take_packed in upload_inputs; the upload's last
//   write event is recorded) -> Free (reclaim_packed, once that event is
//   CL_COMPLETE). A Packed block whose batch retires without an upload goes
//   straight back to Free; no command ever read it.
// A non-blocking write's host memory stays unchanged until the write completes,
// because a block is Free again only after its last write completed and only a
// Free block is handed to a producer. Producers touch only their own Packing
// block and the free list; the spec fields are written once in init_pack_pool,
// before any producer thread exists.
struct SurvivorRunnerTransfer::PackPool {
    enum class St : unsigned char { Free, Packing, Packed, InFlight };
    struct Block {
        cl_mem buf = nullptr;            // pinned: CL_MEM_ALLOC_HOST_PTR, mapped once
        unsigned char* host = nullptr;   // mapped pointer (pinned) or heap.data()
        std::vector<unsigned char> heap; // pageable block when pinning failed
        St st = St::Free;
        std::uint64_t gen = 0;
        cl_event ev = nullptr; // InFlight: last write that reads this block
        int nw = 0;
        StagedWrite w[8];
        cl_uint bits_try = 0; // trim bits the producer tried
        cl_uint bits = 0;     // trim bits of the packed layout
        const void* ptr = nullptr;
        std::size_t n = 0;
        cl_uint4 first{};
        cl_uint4 last{};
    };
    // Spec (read-only after init_pack_pool).
    bool pinned = false;
    cl_uint trim_base = 0; // 1 when the f-table trim applies to every batch
    cl_uint fmin = 0;
    std::size_t fcount = 0;
    std::size_t bytes = 0; // per block
    std::vector<Block> blocks;
    mutable std::mutex mtx;
    std::vector<int> free_list; // guarded by mtx
    PackPoolStats stats;        // counters guarded by mtx
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_INTERNAL_HPP
