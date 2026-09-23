#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_INLINE_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_INLINE_HPP

// Inline member functions of SurvivorRunner and of its state base classes
// (survivor_runtime/state_*.hpp). survivor_runtime.hpp includes this header
// after the class definitions.

#include "survivor_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace collatz::gpu {

inline const std::string& SurvivorRunnerCpi::tail_desc() const noexcept {
    return tail_desc_;
}

inline bool SurvivorRunnerTransfer::pinned_enabled() const noexcept {
    return !xfer_.pinned_failed;
}

inline bool SurvivorRunnerTransfer::copy_queue_ready() const noexcept {
    return xfer_.copy_q != nullptr;
}

inline bool SurvivorRunnerTransfer::upload_trim_enabled() const noexcept {
    return xfer_.ftab != nullptr;
}

inline std::uint64_t SurvivorRunnerTransfer::prefetch_hits() const noexcept {
    return xfer_.pf_hits;
}

inline std::uint64_t SurvivorRunnerTransfer::prefetch_issued() const noexcept {
    return xfer_.pf_issued;
}

inline const SurvivorRunnerTransfer::PipelineStats&
SurvivorRunnerTransfer::pipeline_stats() const noexcept {
    return pipe_;
}

inline void SurvivorRunnerCpi::set_cand_budget_cap(std::size_t cap) noexcept {
    cand_budget_cap_ = cap;
}

inline std::uint64_t SurvivorRunnerTransfer::input_check_failures() const noexcept {
    return xfer_.chk_fail;
}

inline std::uint64_t SurvivorRunnerChecks::mask_wg_failures() const noexcept {
    return mask_wg_fail_;
}

inline std::uint64_t SurvivorRunnerTransfer::trimmed_batches() const noexcept {
    return xfer_.trim_batches;
}

inline std::uint64_t SurvivorRunnerTransfer::trimmed_m64() const noexcept {
    return xfer_.trim_m64;
}

inline void SurvivorRunnerChecks::set_selfcheck_inject(std::uint64_t first,
                                                       std::uint64_t count) noexcept {
    sc_inject_first_ = first;
    sc_inject_count_ = count;
}

inline bool SurvivorRunnerChecks::selfcheck_inject_armed() const noexcept {
    return sc_inject_count_ != 0;
}

inline std::uint64_t SurvivorRunnerTransfer::inflight_discarded() const noexcept {
    return infl_.discarded;
}

inline std::uint64_t SurvivorRunnerTransfer::inflight_batches() const noexcept {
    return infl_.batches;
}

inline double SurvivorRunnerTransfer::inflight_wait_seconds() const noexcept {
    return infl_.wait_ns / 1e9;
}

inline const DeviceInfo& SurvivorRunnerDevice::device() const noexcept {
    return info_;
}

inline double SurvivorRunnerDevice::kernel_seconds() const noexcept {
    return kernel_ns_ / 1e9;
}

inline double SurvivorRunnerDevice::transfer_seconds() const noexcept {
    return transfer_ns_ / 1e9;
}

inline double SurvivorRunnerDevice::write_wall_seconds() const noexcept {
    return write_wall_ns_ / 1e9;
}

inline double SurvivorRunnerDevice::kernel_wall_seconds() const noexcept {
    return kernel_wall_ns_ / 1e9;
}

inline double SurvivorRunnerDevice::read_wall_seconds() const noexcept {
    return read_wall_ns_ / 1e9;
}

inline double SurvivorRunnerCpi::cpi_pass2_wall_seconds() const noexcept {
    return cpi_pass2_wall_ns_ / 1e9;
}

inline std::uint64_t SurvivorRunnerCpi::device_count_batches() const noexcept {
    return dc_batches_;
}

inline std::size_t SurvivorRunnerCpi::persist_chunk() const noexcept {
    return persist_chunk_;
}

inline std::size_t SurvivorRunnerCpi::persist_mult() const noexcept {
    return persist_mult_;
}

inline std::size_t SurvivorRunnerCpi::mask_local_max() const noexcept {
    return mask_local_max_;
}

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_INLINE_HPP
