#ifndef COLLATZ_GPU_OPENCL_RUNTIME_HPP
#define COLLATZ_GPU_OPENCL_RUNTIME_HPP

// OpenCL 1.2 host runtime: device enumeration and choice, and the 16-step block
// table that the survivor pipeline (survivor_runtime.hpp) uploads to the device.
//
// OpenCLError names the failing call and the decoded cl_int status. Core
// OpenCL 1.2 C only: no CUDA or vendor extensions.

#include "collatz/gpu/opencl.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace collatz::gpu {

/// Thrown on any OpenCL failure with a decoded, human-readable message.
class OpenCLError : public std::runtime_error {
public:
    explicit OpenCLError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Decode a cl_int status code into a short symbolic string (e.g. "CL_SUCCESS",
/// "CL_OUT_OF_RESOURCES"). Unknown codes render as their integer value.
[[nodiscard]] std::string cl_error_string(cl_int status);

/// Static description of one OpenCL device.
struct DeviceInfo {
    std::string platform_name;
    std::string device_name;
    std::string device_version; ///< CL_DEVICE_VERSION (e.g. "OpenCL 1.2 ...").
    cl_device_type device_type = 0;
    cl_uint address_bits = 0;
    cl_ulong max_mem_alloc_size = 0; ///< CL_DEVICE_MAX_MEM_ALLOC_SIZE (bytes).
    cl_ulong global_mem_size = 0;    ///< CL_DEVICE_GLOBAL_MEM_SIZE (bytes).
    cl_uint max_compute_units = 0;
    bool host_unified_memory = false; ///< CL_DEVICE_HOST_UNIFIED_MEMORY: an integrated GPU.
    bool available = false;           ///< CL_DEVICE_AVAILABLE and CL_DEVICE_COMPILER_AVAILABLE.
    bool candidate = false;           ///< An available GPU, which a --gpu run may use.
    bool selected = false;            ///< The device a --gpu run tries first.
    cl_device_id id = nullptr;
};

/// Every OpenCL device on every platform, in the order of device_order
/// (device_order.hpp): the available GPUs, those without host-unified memory
/// first and then by larger global memory, then the other devices. `selected`
/// marks the first device of gpu_choice(). Empty, without throwing, when there
/// is none. `collatz gpu-info` prints this list, and COLLATZ_GPU_DEVICE takes
/// its indices.
[[nodiscard]] std::vector<DeviceInfo> enumerate_devices();

/// The GPUs a --gpu run tries, in order: every candidate of enumerate_devices(),
/// or only the device whose index COLLATZ_GPU_DEVICE gives. `why` says why the
/// list is empty.
struct GpuChoice {
    std::vector<DeviceInfo> tries;
    std::string why;
};
[[nodiscard]] GpuChoice gpu_choice();

/// CL_DEVICE_NAME of the device a --gpu run tries first, for the "gpu device:"
/// line `collatz worker` shows; nullopt when there is none. Read-only: creates
/// no context.
[[nodiscard]] std::optional<std::string> selected_device_name();

/// The 2^16-entry table16, packed as e = (c << 5) | f with c = T^16(r) and f
/// the number of odd steps among those 16. Index by the low 16 bits of v.
[[nodiscard]] std::array<std::uint32_t, 1u << 16> build_tbl16();

} // namespace collatz::gpu

#endif // COLLATZ_GPU_OPENCL_RUNTIME_HPP
