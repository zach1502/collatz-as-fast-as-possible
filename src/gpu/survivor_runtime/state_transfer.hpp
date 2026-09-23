#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_TRANSFER_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_TRANSFER_HPP

// The transfer-path, in-flight pipeline and producer-pack state of
// SurvivorRunner (survivor_runtime.hpp) and the accessors that read it.
// SurvivorRunner derives from this class, so its code names these members and
// the survivor_detail aliases directly.

#include "collatz/gpu/opencl.hpp"
#include "survivor_runtime/runner_types.hpp"

#include <cstddef>
#include <cstdint>

namespace collatz::gpu {

/// Transfer-path, in-flight pipeline and producer-pack state of SurvivorRunner.
class SurvivorRunnerTransfer {
public:
    // Dispatch-thread transfer path; none of it changes results.
    //  Pinned staging: run_cpi uploads go through two staging slots in pinned
    //    host memory (CL_MEM_ALLOC_HOST_PTR, mapped once, grow-only), or in
    //    pageable memory when pinning fails; a slot is refilled only after the
    //    last write that read it has completed.
    //  Copy queue: a third in-order queue carries only the host->device input
    //    copies of the next batch, into a second device input set
    //    (prefetch_inputs); kernels stay on queue_ and wait on the copy event.
    //    Without it (the driver refuses the queue) uploads run on queue_.
    //  Upload trim: run_cpi uploads n0 and m as u64 when their high halves are
    //    zero, and the f-dependent fields as a 1-byte index into a per-run
    //    table (set_trim_tables).
    [[nodiscard]] bool pinned_enabled() const noexcept;
    [[nodiscard]] bool copy_queue_ready() const noexcept;
    [[nodiscard]] bool upload_trim_enabled() const noexcept;
    // Prefetched batches consumed (hits) and issued.
    [[nodiscard]] std::uint64_t prefetch_hits() const noexcept;
    [[nodiscard]] std::uint64_t prefetch_issued() const noexcept;
    using PipelineStats = survivor_detail::PipelineStats;
    [[nodiscard]] const PipelineStats& pipeline_stats() const noexcept;
    using PackPoolStats = survivor_detail::PackPoolStats; // SurvivorRunner::pack_pool_stats()
    [[nodiscard]] std::uint64_t input_check_failures() const noexcept;
    [[nodiscard]] std::uint64_t trimmed_batches() const noexcept;
    /// Trimmed batches in which m was also narrowed to u64 (only when every m < 2^64).
    [[nodiscard]] std::uint64_t trimmed_m64() const noexcept;
    [[nodiscard]] std::uint64_t inflight_discarded() const noexcept;
    // Batches that went through cpi_submit, and host seconds cpi_complete spent
    // waiting for a slot's readback.
    [[nodiscard]] std::uint64_t inflight_batches() const noexcept;
    [[nodiscard]] double inflight_wait_seconds() const noexcept;

protected:
    // Transfer-path state, one aggregate so the manual move stays one statement.
    static constexpr std::size_t kInputCheckRows = survivor_detail::kInputCheckRows;
    using StagingSlot = survivor_detail::StagingSlot;
    using InputSet = survivor_detail::InputSet;
    using InputMember = survivor_detail::InputMember;
    using StagedWrite = survivor_detail::StagedWrite;
    using PrePack = survivor_detail::PrePack;
    using TransferState = survivor_detail::TransferState;
    TransferState xfer_;

    using InflightSlot = survivor_detail::InflightSlot;
    using InflightState = survivor_detail::InflightState;
    InflightState infl_;
    PipelineStats pipe_; // host-pipeline settings and counters
    struct PackPool;     // producer-pack pool (survivor_runtime/internal.hpp); owned
    PackPool* pack_pool_ = nullptr;
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_TRANSFER_HPP
