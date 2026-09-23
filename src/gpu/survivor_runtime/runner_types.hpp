#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_TYPES_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_TYPES_HPP

// The in-flight slot layout and the types of SurvivorRunner's state and
// statistics. SurvivorRunnerTransfer (state_transfer.hpp) aliases the
// survivor_detail types, so SurvivorRunner::PipelineStats,
// SurvivorRunner::TransferState and so on name them.

#include "collatz/gpu/opencl.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace collatz::gpu {

/// Byte offsets, inside an in-flight slot's host area, of every word the slot
/// reads back. The input-check rows follow the control words.
struct SlotLayout {
    std::size_t candcnt = 0, candovf = 0, sc = 0, fc = 0, fo = 0, gcov = 0, gdig = 0, gblk = 0,
                gchk = 0, gmax = 0, gcnt = 0, chk = 0, total = 0;
    // Host offset and entry capacity of the flag-list prefix.
    std::size_t flags = 0, flags_n = 0;
};
inline constexpr std::size_t kSlotCheckRows = 5; // must equal SurvivorRunner::kInputCheckRows

namespace survivor_detail {

/// Host-pipeline settings and counters, printed on the stderr `gpu pipeline:`
/// line.
struct PipelineStats {
    std::size_t flag_k = 0;           // flag-list prefix entries
    std::uint64_t pf_early_n = 0;     // early prefetches issued without a wait
    std::uint64_t pf_early_wait = 0;  // ... whose marker had to be host-waited
    std::uint64_t flag_prefix_ok = 0; // flagged batches served by the prefix
    std::uint64_t flag_rest_rd = 0;   // flagged batches needing a 2nd read
    std::uint64_t cand_cap_ids = 0;   // effective id cap (last batch)
    std::uint64_t cupper_hints = 0;   // submits that used the producer's cupper
    std::uint64_t prepacks = 0;       // staging slots packed ahead
    std::uint64_t prepack_hits = 0;   // ... consumed by their prefetch
    std::uint64_t prepack_lost = 0;   // ... dropped (slot reused first)
    std::uint64_t prepack_busy = 0;   // ... skipped: slot's last write still running
};

struct PackPoolStats {
    std::size_t blocks = 0;      // pool size
    std::size_t block_bytes = 0; // bytes per block
    bool pinned = false;         // blocks are CL_MEM_ALLOC_HOST_PTR
    std::uint64_t packed = 0;    // batches packed on a producer
    std::uint64_t hits = 0;      // ... uploaded from their block
    std::uint64_t nopool = 0;    // producer found no free block
    std::uint64_t nofit = 0;     // packed layout larger than a block
    std::uint64_t mismatch = 0;  // upload-time trim bits differ (fallback)
    std::uint64_t unused = 0;    // retired without an upload from the block
    std::uint64_t stale = 0;     // stale block reference (re-upload)
};

/// Rows of the prefetched-input readback check (see TransferState::chk).
inline constexpr std::size_t kInputCheckRows = 5;
struct StagingSlot {
    cl_mem buf = nullptr;            // pinned: CL_MEM_ALLOC_HOST_PTR buffer, mapped once
    unsigned char* host = nullptr;   // mapped pointer (pinned) or heap.data()
    std::vector<unsigned char> heap; // pageable staging when pinning fails
    std::size_t bytes = 0;
    cl_event ev = nullptr; // last write that reads this slot (null = free)
};
struct InputSet {
    cl_mem n0 = nullptr, m = nullptr, astar = nullptr, inv3f = nullptr, bvbase = nullptr,
           bvmode = nullptr, p3f = nullptr, fidx = nullptr;
    std::size_t cap = 0;
};
// One staged write. The destination is an input-set member, resolved against
// the target set when the write is enqueued, since prefetch_prepack packs
// early.
using InputMember = cl_mem InputSet::*;
struct StagedWrite {
    InputMember buf = nullptr;
    std::size_t bytes = 0;
    const void* host = nullptr;
    const char* what = "";
};
// A staging slot packed ahead of its prefetch_inputs (prefetch_prepack).
struct PrePack {
    bool valid = false;
    int slot = 0;
    int nw = 0;
    StagedWrite w[8];
    cl_uint bits = 0;
    const void* ptr = nullptr;
    std::size_t n = 0;
    cl_uint4 first{};
    cl_uint4 last{};
};
struct TransferState {
    bool pinned_failed = false; // pinned staging failed; slots use pageable memory
    cl_command_queue copy_q = nullptr;
    StagingSlot stage[2];
    int next_stage = 0;
    // The idle device input set (the active set is n0_buf_... + fidx below).
    InputSet alt;
    cl_mem act_fidx = nullptr;   // active set's f-index buffer (trim)
    std::size_t act_cap = 0;     // capacity of the active input handles
    std::size_t out_cap = 0;     // capacity of run()'s output buffers
    cl_event alt_free = nullptr; // marker on queue_ after the last reader of `alt`
    // Pending prefetch (copied into `alt`, not yet consumed).
    bool pf_pending = false;
    const void* pf_ptr = nullptr;
    std::size_t pf_n = 0;
    cl_uint4 pf_first{};
    cl_uint4 pf_last{};
    cl_uint pf_bits = 0;
    cl_event pf_ev = nullptr;
    std::uint64_t pf_seq = 0;
    PrePack prepack;          // prefetch_prepack
    bool inject_copy = false; // COLLATZ_GPU_SELFCHECK_INJECT_COPY test hook
    // Input readback check of a consumed prefetch; the read targets live
    // here. Rows: n0[0], n0[n-1], m[n-1], astar[n-1] and fidx[n-1].
    alignas(16) unsigned char chk[kInputCheckRows][16] = {};
    unsigned char chk_want[kInputCheckRows][16] = {};
    std::size_t chk_len[kInputCheckRows] = {};
    bool chk_on = false;
    // Upload-trim tables.
    unsigned fmin = 0;
    std::size_t fcount = 0;
    cl_mem ftab = nullptr;  // uint4 {inv3f, bvbase, bvmode, 0}
    cl_mem p3tab = nullptr; // uint4 3^f
    cl_uint cur_bits = 0;   // trim bits of the batch in flight (tail binds them)
    std::uint64_t pf_hits = 0, pf_issued = 0, chk_fail = 0, trim_batches = 0, trim_m64 = 0;
};

// Two-deep pipeline state, one slot per batch in flight. Every buffer the
// host reads back is per slot (control words, per-group partials and
// records, flag list), so batch i+1's commands never overwrite a word that
// batch i's host fold still needs. The candidate-id list cpi_cand_buf_ is
// shared; cpi_submit explains why the in-order queue makes that safe.
struct InflightSlot {
    SlotLayout lay;
    cl_mem candcnt = nullptr, candovf = nullptr, sc = nullptr, fc = nullptr, fo = nullptr,
           wc = nullptr, gcov = nullptr, gdig = nullptr, gblk = nullptr, gcnt = nullptr,
           gmax = nullptr, gchk = nullptr, flaglist = nullptr;
    std::size_t groups = 0;   // allocated per-group capacity
    std::size_t flag_cap = 0; // allocated flag-list capacity
    // Readback target: a CL_MEM_ALLOC_HOST_PTR buffer mapped once (pinned),
    // so a non-blocking read does not block on a discrete device.
    cl_mem host_buf = nullptr;
    unsigned char* host = nullptr;
    std::vector<unsigned char> heap; // pageable fallback if pinning fails
    // The pending batch (valid while busy).
    bool busy = false;
    cl_event ev = nullptr; // the slot's last readback (in-order: all landed)
    cl_uint nonce = 0;
    std::uint64_t cupper = 0;
    std::size_t cand_cap = 0;
    std::size_t tail_groups = 0;
    cl_uint mask_groups = 0;
    std::uint32_t flag_cap_used = 0;
    std::size_t flag_pref = 0; // flag-list entries read with the slot
    bool inject = false;       // injection hook still armed: corrupt the readback
    bool chk_on = false;       // input-check rows were read
    unsigned char chk_want[kInputCheckRows][16] = {};
    std::size_t chk_len[kInputCheckRows] = {};
};
struct InflightState {
    InflightSlot slot[2];
    int next = 0;
    std::uint64_t batches = 0;
    std::uint64_t discarded = 0;
    double wait_ns = 0.0;
    // Host scratch for sc_check_tail (grow-only).
    std::vector<cl_ulong> gcov, gdig, gblk, gchk;
    std::vector<cl_uint> gcnt;
    std::vector<cl_uint4> gmax;
};

} // namespace survivor_detail
} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_TYPES_HPP
