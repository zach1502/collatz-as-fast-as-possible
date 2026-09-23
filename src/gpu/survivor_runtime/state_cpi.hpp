#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_CPI_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_CPI_HPP

// The candidate-per-item (cpi) program, kernels, buffers and tail settings of
// SurvivorRunner (survivor_runtime.hpp) and the accessors that read them.
// SurvivorRunner derives from this class, so its code names these members
// directly.

#include "collatz/gpu/opencl.hpp"
#include "tail_defaults.hpp" // kPersistChunk, kPersistMult

#include <cstddef>
#include <cstdint>
#include <string>

namespace collatz::gpu {

/// Candidate-per-item program, kernel, buffer and tail state of SurvivorRunner.
class SurvivorRunnerCpi {
public:
    // Tail variant of the cpi build: "u64n=.. split=..".
    [[nodiscard]] const std::string& tail_desc() const noexcept;
    /// Lower the id cap so the buffers fit CL_DEVICE_GLOBAL_MEM_SIZE (called by
    /// verify_gpu). Only the frequency of the candidate-overflow split changes;
    /// every Result is bit-identical.
    void set_cand_budget_cap(std::size_t cap) noexcept;
    // Host wall time of tail_persistent, from its enqueue to the last blocking
    // readback. Diagnostic.
    [[nodiscard]] double cpi_pass2_wall_seconds() const noexcept;
    // Device-mask batches whose candidate count stayed on the device.
    [[nodiscard]] std::uint64_t device_count_batches() const noexcept;
    // Persistent-lane tail: its chunk and its grid multiple.
    [[nodiscard]] std::size_t persist_chunk() const noexcept;
    [[nodiscard]] std::size_t persist_mult() const noexcept;
    // Largest work-group the device mask can launch with on this device. The
    // mask needs 2^A/32 work-items per group, so verify_gpu uses the CPU for
    // an A above it.
    [[nodiscard]] std::size_t mask_local_max() const noexcept;

protected:
    // Candidate-per-item program and kernels, sharing this runner's context,
    // queue, tbl16 and bitvector buffers.
    cl_program program_cpi_ = nullptr;
    // The coalesced device mask with work-group-aggregated scatter
    // (cpi_mask_scatter_coalesced_wgscan), one work-group per survivor: one
    // coalesced window load, a __local straddle exchange, and one global atomic
    // on cand_counter and two on sc_out per survivor.
    cl_kernel kern_mask_ = nullptr;
    cl_kernel kern_persistent_ = nullptr; // tail_persistent (persistent-lane tail)
    // CL_KERNEL_WORK_GROUP_SIZE of kern_mask_, at most
    // CL_DEVICE_MAX_WORK_ITEM_SIZES[0] (Setup::cpi_kernels).
    std::size_t mask_local_max_ = 0;
    std::uint64_t dc_batches_ = 0;
    // cpi buffers reused across batches.
    std::size_t cpi_cap_cands_ = 0;
    std::size_t cpi_cap_groups_ = 0;
    cl_mem cpi_candcnt_buf_ = nullptr;   // 1 uint (atomic write cursor -> C)
    cl_mem cpi_candovf_buf_ = nullptr;   // 1 uint (cand_out capacity exceeded)
    cl_mem cpi_cand_buf_ = nullptr;      // cands uints (packed ids)
    cl_mem cpi_gcov_buf_ = nullptr;      // groups ulongs
    cl_mem cpi_gdig_buf_ = nullptr;      // groups ulongs (pow digest partials)
    cl_mem cpi_gblk_buf_ = nullptr;      // groups ulongs
    cl_mem cpi_gcnt_buf_ = nullptr;      // groups uints
    cl_mem cpi_gmax_buf_ = nullptr;      // groups uint4
    cl_mem cpi_flagcount_buf_ = nullptr; // 1 uint (atomic counter)
    cl_mem cpi_flaglist_buf_ = nullptr;  // flag_cap uints
    cl_mem cpi_flagovf_buf_ = nullptr;   // 1 uint
    std::size_t cpi_flag_cap_ = 0;
    std::size_t cpi_cap_flag_ = 0; // allocated flag-list capacity (grows)
    // Persistent-lane tail: one global work cursor, the chunk of candidates a
    // lane claims at a time and the grid multiple (work-groups per compute
    // unit).
    cl_mem cpi_workcnt_buf_ = nullptr; // 1 uint (atomic work cursor)
    std::size_t persist_chunk_ = kPersistChunk;
    std::size_t persist_mult_ = kPersistMult;
    // The tail_persistent variant the cpi program was built with
    // (COLLATZ_GPU_TAIL_U64N needs every n below 2^64).
    bool tail_u64n_ = false;
    std::string tail_desc_;
    std::size_t cand_budget_cap_ = 0; // device-memory id cap (0 = none)
    double cpi_pass2_wall_ns_ = 0.0;

    // Second in-order queue on the same context, for the pipeline's flag-list
    // reads (see create()); nullptr when the driver refuses a second queue.
    cl_command_queue queue_alt_ = nullptr;
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_CPI_HPP
