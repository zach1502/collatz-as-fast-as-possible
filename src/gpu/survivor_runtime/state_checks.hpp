#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_CHECKS_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_CHECKS_HPP

// The self-check state of SurvivorRunner (survivor_runtime.hpp) and the
// accessors that read it. SurvivorRunner
// derives from this class, so its code names these members directly.

#include "collatz/gpu/opencl.hpp"

#include <cstdint>

namespace collatz::gpu {

/// Self-check state of SurvivorRunner.
class SurvivorRunnerChecks {
public:
    /// Batches rejected because a mask work-group did not report (sc word 3 !=
    /// the survivor or item count it covers).
    [[nodiscard]] std::uint64_t mask_wg_failures() const noexcept;
    /// Test hook: corrupt the host-side readback of `count` self-checked
    /// dispatches, starting at dispatch index `first` (0-based, in dispatch order),
    /// so a test can see the self-check catch them.
    void set_selfcheck_inject(std::uint64_t first, std::uint64_t count) noexcept;
    [[nodiscard]] bool selfcheck_inject_armed() const noexcept;

protected:
    cl_uint sc_nonce_ = 0;
    std::uint64_t sc_seq_ = 0;
    std::uint64_t sc_inject_first_ = ~static_cast<std::uint64_t>(0);
    std::uint64_t sc_inject_count_ = 0;
    bool sc_inject_now_ = false;
    cl_uint sc_cur_nonce_ = 0;      // nonce of the dispatch in flight (pass 2 reads it)
    cl_mem sc_words_buf_ = nullptr; // 4 uints: {idsum, nonce, count, 0} from the mask
    cl_mem cpi_gchk_buf_ = nullptr; // 4 ulongs per tail group
    cl_mem chk_buf_ = nullptr;      // 1 ulong per survivor_tail16 item

    // Test hook (COLLATZ_GPU_SELFCHECK_INJECT_EXPAND=1): the injected dispatch
    // corrupts the mask's deferred count word instead of the tail readback.
    bool sc_inject_expand_ = false;
    // Test hook (COLLATZ_GPU_SELFCHECK_INJECT_LOST=1): an injected
    // tail_persistent dispatch skips its launch, so its records are left over
    // from an earlier launch, as with a silently lost kernel.
    bool sc_inject_lost_ = false;
    // Test hook (COLLATZ_GPU_SELFCHECK_INJECT_DROPWG=1): the cpi program is
    // built with -DCOLLATZ_GPU_INJECT_DROPWG and an injected mask dispatch has
    // its last work-group return without doing anything, which is
    // what a work-group lost under GPU contention looks like. Arms sc word 4.
    bool sc_inject_dropwg_ = false;
    // Batches rejected by the mask work-group count check.
    std::uint64_t mask_wg_fail_ = 0;
    // Test hook (COLLATZ_GPU_INJECT_CL_FAIL=<k>, k >= 1): the k-th tail launch
    // throws the OpenCLError of a failed enqueue (CL_OUT_OF_RESOURCES) instead
    // of being enqueued, as a device fault after setup would. 0 is off.
    std::uint64_t cl_fail_at_ = 0;
    std::uint64_t tail_launches_ = 0;
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_CHECKS_HPP
