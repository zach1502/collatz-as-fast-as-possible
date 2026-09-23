#pragma once

// The order in which `verify --gpu` tries the OpenCL devices and in which
// `collatz gpu-info` lists them, and the COLLATZ_GPU_DEVICE override. These
// need no OpenCL, so a unit test can check them without a device.

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace collatz::gpu {

/// What the device order depends on, from clGetDeviceInfo.
struct DeviceRankKey {
    bool gpu = false;             ///< CL_DEVICE_TYPE_GPU is set
    bool available = false;       ///< CL_DEVICE_AVAILABLE and CL_DEVICE_COMPILER_AVAILABLE
    bool unified = false;         ///< CL_DEVICE_HOST_UNIFIED_MEMORY (an integrated GPU)
    std::uint64_t global_mem = 0; ///< CL_DEVICE_GLOBAL_MEM_SIZE in bytes
};

[[nodiscard]] constexpr bool gpu_candidate(const DeviceRankKey& d) noexcept {
    return d.gpu && d.available;
}

/// Indices of `devs` in listing order: the candidates first, those without
/// host-unified memory before the others and then by larger global memory,
/// ties in enumeration order; then every other device in enumeration order.
/// A run tries the candidates front to back.
[[nodiscard]] inline std::vector<std::size_t> device_order(const std::vector<DeviceRankKey>& devs) {
    std::vector<std::size_t> idx(devs.size());
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    std::stable_sort(idx.begin(), idx.end(), [&devs](std::size_t x, std::size_t y) {
        const DeviceRankKey& a = devs[x];
        const DeviceRankKey& b = devs[y];
        if (gpu_candidate(a) != gpu_candidate(b)) {
            return gpu_candidate(a);
        }
        if (!gpu_candidate(a)) {
            return false; // the other devices keep their enumeration order
        }
        if (a.unified != b.unified) {
            return !a.unified;
        }
        return a.global_mem > b.global_mem;
    });
    return idx;
}

/// COLLATZ_GPU_DEVICE: the gpu-info index of the one device a --gpu run uses.
struct DeviceRequest {
    bool set = false;   ///< the variable is set and not empty
    bool valid = false; ///< it is a decimal index
    std::size_t index = 0;
};

[[nodiscard]] inline DeviceRequest parse_device_request(const char* value) {
    DeviceRequest r;
    if (value == nullptr || *value == '\0') {
        return r;
    }
    r.set = true;
    if (*value < '0' || *value > '9') {
        return r; // strtoull would accept a sign or blanks
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(value, &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return r;
    }
    r.valid = true;
    r.index = static_cast<std::size_t>(v);
    return r;
}

} // namespace collatz::gpu
