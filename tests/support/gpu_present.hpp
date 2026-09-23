#pragma once

// Whether the machine has an OpenCL GPU, and how a GPU test reports a case
// that could not run on one. The case records a skip (skip_gpu_case, with a
// MESSAGE) and returns; the executable's main (gpu_test_main.hpp) then exits
// with kGpuSkipCode, which CTest reports as skipped (SKIP_RETURN_CODE of the
// GPU entries in tests/CMakeLists.txt) instead of passed.
#include "opencl_runtime.hpp" // collatz::gpu::enumerate_devices

#include <doctest/doctest.h>
#include <string>

namespace collatz::test {

// The CTest and automake convention for a skipped test.
inline constexpr int kGpuSkipCode = 77;

// An available GPU, the only kind of device a --gpu run uses.
[[nodiscard]] inline bool gpu_present() {
    for (const collatz::gpu::DeviceInfo& d : collatz::gpu::enumerate_devices()) {
        if (d.candidate) {
            return true;
        }
    }
    return false;
}

// Skips recorded by the cases of this executable.
[[nodiscard]] inline unsigned& gpu_skip_count() {
    static unsigned n = 0;
    return n;
}

inline void skip_gpu_case(const std::string& why) {
    ++gpu_skip_count();
    MESSAGE("SKIPPED: " << why);
}

// True, after recording a skip, when the machine has no available OpenCL GPU.
[[nodiscard]] inline bool skip_without_gpu() {
    if (gpu_present()) {
        return false;
    }
    skip_gpu_case("no available OpenCL GPU present");
    return true;
}

// Whether verify_gpu ran on the device (`used`). A run that fell back to the
// CPU records a skip with verify_gpu's reason, so the case cannot pass
// without the GPU path.
[[nodiscard]] inline bool gpu_ran(bool used, const std::string& msg) {
    if (!used) {
        skip_gpu_case("the GPU path fell back to the CPU: " + msg);
    }
    return used;
}

} // namespace collatz::test
