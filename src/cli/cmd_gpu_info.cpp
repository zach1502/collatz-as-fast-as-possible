#include "collatz/cli.hpp"

#include <cstddef>
#include <cstdio>
#include <vector>

// gpu-info lists the OpenCL 1.2 devices (with COLLATZ_HAS_OPENCL). The command
// is registered in every build, so the command list does not depend on the
// backend.

#if defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL
#include "opencl_runtime.hpp"
#endif

namespace {

#if defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL
const char* device_type_name(cl_device_type type) {
    if ((type & CL_DEVICE_TYPE_GPU) != 0) {
        return "GPU";
    }
    if ((type & CL_DEVICE_TYPE_CPU) != 0) {
        return "CPU";
    }
    if ((type & CL_DEVICE_TYPE_ACCELERATOR) != 0) {
        return "ACCELERATOR";
    }
    return "OTHER";
}

void print_opencl() {
    const std::vector<collatz::gpu::DeviceInfo> devices = collatz::gpu::enumerate_devices();
    if (devices.empty()) {
        std::printf("gpu-info [opencl]: no OpenCL device found\n");
        return;
    }
    // The order is the one --gpu tries the GPUs in, and COLLATZ_GPU_DEVICE
    // takes these indices.
    std::printf("gpu-info [opencl]: %zu OpenCL device(s)\n", devices.size());
    bool any_selected = false;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const collatz::gpu::DeviceInfo& d = devices[i];
        any_selected = any_selected || d.selected;
        std::printf("  [%zu] %s / %s%s\n",
                    i,
                    d.platform_name.c_str(),
                    d.device_name.c_str(),
                    d.selected ? "  (selected for --gpu)" : "");
        std::printf("       type=%s  version=\"%s\"\n",
                    device_type_name(d.device_type),
                    d.device_version.c_str());
        std::printf(
            "       address_bits=%u  compute_units=%u\n", d.address_bits, d.max_compute_units);
        std::printf("       max_mem_alloc=%.2f MiB  global_mem=%.2f MiB\n",
                    static_cast<double>(d.max_mem_alloc_size) / (1024.0 * 1024.0),
                    static_cast<double>(d.global_mem_size) / (1024.0 * 1024.0));
        std::printf("       unified_memory=%s  available=%s%s\n",
                    d.host_unified_memory ? "yes" : "no",
                    d.available ? "yes" : "no",
                    d.candidate ? "" : "  (not used by --gpu)");
    }
    if (!any_selected) {
        std::printf("  --gpu uses the CPU: %s\n", collatz::gpu::gpu_choice().why.c_str());
    }
}
#endif // COLLATZ_HAS_OPENCL

int run_gpu_info(int /*argc*/, char** /*argv*/) {
#if defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL
    print_opencl();
#else
    std::printf("gpu-info: built without the GPU backend (COLLATZ_OPENCL=OFF)\n");
#endif
    return 0;
}

} // namespace

COLLATZ_REGISTER_COMMAND("gpu-info", "list the OpenCL devices", run_gpu_info)
