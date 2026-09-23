#include "opencl_runtime.hpp"

#include "collatz/collatz_fn.hpp"
#include "collatz/u128.hpp"
#include "device_order.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace collatz::gpu {

namespace {

[[nodiscard]] std::string device_string(cl_device_id dev, cl_device_info param) {
    std::size_t size = 0;
    if (clGetDeviceInfo(dev, param, 0, nullptr, &size) != CL_SUCCESS || size == 0) {
        return {};
    }
    std::string out(size, '\0');
    if (clGetDeviceInfo(dev, param, size, out.data(), nullptr) != CL_SUCCESS) {
        return {};
    }
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] std::string platform_string(cl_platform_id plat, cl_platform_info param) {
    std::size_t size = 0;
    if (clGetPlatformInfo(plat, param, 0, nullptr, &size) != CL_SUCCESS || size == 0) {
        return {};
    }
    std::string out(size, '\0');
    if (clGetPlatformInfo(plat, param, size, out.data(), nullptr) != CL_SUCCESS) {
        return {};
    }
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

template <typename T>
[[nodiscard]] T device_scalar(cl_device_id dev, cl_device_info param) {
    T value{};
    clGetDeviceInfo(dev, param, sizeof(T), &value, nullptr);
    return value;
}

[[nodiscard]] std::vector<cl_platform_id> platforms() {
    cl_uint count = 0;
    if (clGetPlatformIDs(0, nullptr, &count) != CL_SUCCESS || count == 0) {
        return {};
    }
    std::vector<cl_platform_id> plats(count);
    if (clGetPlatformIDs(count, plats.data(), nullptr) != CL_SUCCESS) {
        return {};
    }
    return plats;
}

[[nodiscard]] std::vector<cl_device_id> devices_of(cl_platform_id plat) {
    cl_uint count = 0;
    if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 0, nullptr, &count) != CL_SUCCESS || count == 0) {
        return {};
    }
    std::vector<cl_device_id> devs(count);
    if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, count, devs.data(), nullptr) != CL_SUCCESS) {
        return {};
    }
    return devs;
}

[[nodiscard]] DeviceInfo describe(cl_platform_id plat, cl_device_id dev) {
    DeviceInfo info;
    info.platform_name = platform_string(plat, CL_PLATFORM_NAME);
    info.device_name = device_string(dev, CL_DEVICE_NAME);
    info.device_version = device_string(dev, CL_DEVICE_VERSION);
    info.device_type = device_scalar<cl_device_type>(dev, CL_DEVICE_TYPE);
    info.address_bits = device_scalar<cl_uint>(dev, CL_DEVICE_ADDRESS_BITS);
    info.max_mem_alloc_size = device_scalar<cl_ulong>(dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE);
    info.global_mem_size = device_scalar<cl_ulong>(dev, CL_DEVICE_GLOBAL_MEM_SIZE);
    info.max_compute_units = device_scalar<cl_uint>(dev, CL_DEVICE_MAX_COMPUTE_UNITS);
    info.host_unified_memory =
        device_scalar<cl_bool>(dev, CL_DEVICE_HOST_UNIFIED_MEMORY) != CL_FALSE;
    info.available = device_scalar<cl_bool>(dev, CL_DEVICE_AVAILABLE) != CL_FALSE &&
                     device_scalar<cl_bool>(dev, CL_DEVICE_COMPILER_AVAILABLE) != CL_FALSE;
    info.id = dev;
    return info;
}

[[nodiscard]] DeviceRankKey rank_key(const DeviceInfo& d) {
    return DeviceRankKey{(d.device_type & CL_DEVICE_TYPE_GPU) != 0,
                         d.available,
                         d.host_unified_memory,
                         static_cast<std::uint64_t>(d.global_mem_size)};
}

// Every device of every platform, in listing order (device_order).
[[nodiscard]] std::vector<DeviceInfo> listed_devices() {
    std::vector<DeviceInfo> found;
    std::vector<DeviceRankKey> keys;
    for (const cl_platform_id plat : platforms()) {
        for (const cl_device_id dev : devices_of(plat)) {
            found.push_back(describe(plat, dev));
            keys.push_back(rank_key(found.back()));
            found.back().candidate = gpu_candidate(keys.back());
        }
    }
    std::vector<DeviceInfo> out;
    out.reserve(found.size());
    for (const std::size_t i : device_order(keys)) {
        out.push_back(found[i]);
    }
    return out;
}

[[nodiscard]] GpuChoice choose(const std::vector<DeviceInfo>& devs) {
    GpuChoice c;
    const char* env = std::getenv("COLLATZ_GPU_DEVICE");
    if (const DeviceRequest req = parse_device_request(env); req.set) {
        const std::string tag = std::string("COLLATZ_GPU_DEVICE=") + env;
        if (!req.valid || req.index >= devs.size()) {
            c.why = tag + " names no device (`collatz gpu-info` lists " +
                    std::to_string(devs.size()) + ")";
        } else if (!devs[req.index].candidate) {
            c.why = tag + ": " + devs[req.index].device_name + " is not an available GPU";
        } else {
            c.tries.push_back(devs[req.index]);
        }
        return c;
    }
    for (const DeviceInfo& d : devs) {
        if (d.candidate) {
            c.tries.push_back(d);
        }
    }
    if (c.tries.empty()) {
        c.why = devs.empty() ? std::string("no OpenCL device available")
                             : "no available GPU among the " + std::to_string(devs.size()) +
                                   " OpenCL device(s)";
    }
    return c;
}

} // namespace

std::string cl_error_string(cl_int status) {
    switch (status) {
    case CL_SUCCESS:
        return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND:
        return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE:
        return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE:
        return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
        return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES:
        return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY:
        return "CL_OUT_OF_HOST_MEMORY";
    case CL_MEM_COPY_OVERLAP:
        return "CL_MEM_COPY_OVERLAP";
    case CL_BUILD_PROGRAM_FAILURE:
        return "CL_BUILD_PROGRAM_FAILURE";
    case CL_INVALID_VALUE:
        return "CL_INVALID_VALUE";
    case CL_INVALID_DEVICE:
        return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT:
        return "CL_INVALID_CONTEXT";
    case CL_INVALID_MEM_OBJECT:
        return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_BINARY:
        return "CL_INVALID_BINARY";
    case CL_INVALID_BUILD_OPTIONS:
        return "CL_INVALID_BUILD_OPTIONS";
    case CL_INVALID_PROGRAM:
        return "CL_INVALID_PROGRAM";
    case CL_INVALID_PROGRAM_EXECUTABLE:
        return "CL_INVALID_PROGRAM_EXECUTABLE";
    case CL_INVALID_KERNEL_NAME:
        return "CL_INVALID_KERNEL_NAME";
    case CL_INVALID_KERNEL:
        return "CL_INVALID_KERNEL";
    case CL_INVALID_KERNEL_ARGS:
        return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_WORK_GROUP_SIZE:
        return "CL_INVALID_WORK_GROUP_SIZE";
    case CL_INVALID_WORK_ITEM_SIZE:
        return "CL_INVALID_WORK_ITEM_SIZE";
    case CL_INVALID_BUFFER_SIZE:
        return "CL_INVALID_BUFFER_SIZE";
    default:
        return "CL error " + std::to_string(status);
    }
}

std::vector<DeviceInfo> enumerate_devices() {
    std::vector<DeviceInfo> devs = listed_devices();
    const GpuChoice c = choose(devs);
    for (DeviceInfo& d : devs) {
        d.selected = !c.tries.empty() && d.id == c.tries.front().id;
    }
    return devs;
}

GpuChoice gpu_choice() {
    return choose(listed_devices());
}

std::optional<std::string> selected_device_name() {
    const GpuChoice c = gpu_choice();
    if (c.tries.empty()) {
        return std::nullopt;
    }
    return c.tries.front().device_name;
}

std::array<std::uint32_t, 1u << 16> build_tbl16() {
    std::array<std::uint32_t, 1u << 16> table{};
    for (std::size_t r = 0; r < table.size(); ++r) {
        collatz::u128 v = static_cast<collatz::u128>(r);
        unsigned f = 0;
        for (unsigned j = 0; j < 16; ++j) {
            if ((v & 1) != 0) {
                ++f;
            }
            v = collatz::T(v).value; // r < 2^16 never overflows over 16 steps
        }
        const auto c = static_cast<std::uint32_t>(v); // c = T^16(r) < 3^16 < 2^27
        table[r] = (c << 5) | static_cast<std::uint32_t>(f);
    }
    return table;
}

} // namespace collatz::gpu
