#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_DEVICE_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_DEVICE_HPP

// The device, context, main queue, survivor_tail16 program, Step-2 table, run()
// buffers and transfer times of SurvivorRunner (survivor_runtime.hpp) and the
// accessors that read them. SurvivorRunner derives from this class, so its code
// names these members directly.

#include "collatz/gpu/opencl.hpp"
#include "opencl_runtime.hpp" // DeviceInfo

#include <cstddef>

namespace collatz::gpu {

/// Device, survivor_tail16 and run() state of SurvivorRunner.
class SurvivorRunnerDevice {
public:
    [[nodiscard]] const DeviceInfo& device() const noexcept;
    // Device seconds from CL events over all run() calls: kernel execution, and
    // transfers (reads and writes).
    [[nodiscard]] double kernel_seconds() const noexcept;
    [[nodiscard]] double transfer_seconds() const noexcept;
    // Phase walls of run() from its CL events: the span of the writes, the
    // kernel, and the span of the readbacks. Diagnostic.
    [[nodiscard]] double write_wall_seconds() const noexcept;
    [[nodiscard]] double kernel_wall_seconds() const noexcept;
    [[nodiscard]] double read_wall_seconds() const noexcept;

protected:
    DeviceInfo info_;
    cl_context context_ = nullptr;
    cl_command_queue queue_ = nullptr;
    cl_program program_ = nullptr;
    cl_kernel kernel_ = nullptr;
    cl_mem tbl16_buf_ = nullptr;
    cl_mem bv_bufs_[4] = {nullptr, nullptr, nullptr, nullptr}; // Step-2 table, split.
    int num_bv_buffers_ = 1;                                   // 1..4 live buffers.
    cl_uint bv_buf_words_log2_ = 31;                           // log2 words per buffer.
    double kernel_ns_ = 0.0;
    double transfer_ns_ = 0.0;
    double write_wall_ns_ = 0.0; // span of the writes
    double kernel_wall_ns_ = 0.0;
    double read_wall_ns_ = 0.0; // span of the readbacks

    // Per-item I/O buffers reused across batches, sized to capacity_ items.
    std::size_t capacity_ = 0;
    cl_mem n0_buf_ = nullptr;
    cl_mem m_buf_ = nullptr;
    cl_mem astar_buf_ = nullptr;
    cl_mem inv3f_buf_ = nullptr;
    cl_mem bvbase_buf_ = nullptr;
    cl_mem bvmode_buf_ = nullptr;
    cl_mem p3f_buf_ = nullptr;
    cl_mem cov_buf_ = nullptr;
    cl_mem dig_buf_ = nullptr; // pow digest partials
    cl_mem blocks_buf_ = nullptr;
    cl_mem cand_buf_ = nullptr;
    cl_mem maxv_buf_ = nullptr;
    cl_mem ovf_buf_ = nullptr;
    cl_mem pc_buf_ = nullptr;
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_STATE_DEVICE_HPP
