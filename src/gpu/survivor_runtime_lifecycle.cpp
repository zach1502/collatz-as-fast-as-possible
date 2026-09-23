#include "survivor_runtime/internal.hpp"

namespace collatz::gpu {

using namespace survivor_detail;

SurvivorRunner::~SurvivorRunner() {
    release_transfer(); // drains both queues before the staging/alt sets go
    infl_release();     // unmaps the pinned readback areas
    release_io_buffers();
    release_cpi_buffers();
    for (cl_kernel* k : {&kern_mask_, &kern_persistent_}) {
        if (*k != nullptr) {
            clReleaseKernel(*k);
            *k = nullptr;
        }
    }
    if (program_cpi_ != nullptr) {
        clReleaseProgram(program_cpi_);
        program_cpi_ = nullptr;
    }
    for (int i = 0; i < num_bv_buffers_ && i < 4; ++i) {
        if (bv_bufs_[i] != nullptr) {
            clReleaseMemObject(bv_bufs_[i]);
        }
    }
    for (cl_mem& b : bv_bufs_) {
        b = nullptr;
    }
    if (tbl16_buf_ != nullptr) {
        clReleaseMemObject(tbl16_buf_);
        tbl16_buf_ = nullptr;
    }
    if (kernel_ != nullptr) {
        clReleaseKernel(kernel_);
        kernel_ = nullptr;
    }
    if (program_ != nullptr) {
        clReleaseProgram(program_);
        program_ = nullptr;
    }
    if (queue_ != nullptr) {
        clReleaseCommandQueue(queue_);
        queue_ = nullptr;
    }
    if (queue_alt_ != nullptr) {
        clReleaseCommandQueue(queue_alt_);
        queue_alt_ = nullptr;
    }
    if (context_ != nullptr) {
        clReleaseContext(context_);
        context_ = nullptr;
    }
}

} // namespace collatz::gpu
