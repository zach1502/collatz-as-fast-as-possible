#include "collatz/gpu/kernel_sources.hpp"
#include "host_pipeline.hpp"  // kFlagReadK
#include "opencl_runtime.hpp" // build_tbl16
#include "survivor_runtime/setup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace collatz::gpu {

using namespace survivor_detail;

namespace {

// Program-scope __constant table `split_ctab` (u32 pairs {3^f, c} from
// u128q_split_fill), prepended to the cpi source.
[[nodiscard]] std::string split_table_source() {
    std::vector<std::uint32_t> img(2u * u128q_split_entries());
    u128q_split_fill(img.data());
    std::string src = "__constant uint split_ctab[" + std::to_string(img.size()) + "] = {";
    for (std::size_t i = 0; i < img.size(); ++i) {
        src += (i % 16 == 0 ? "\n" : " ") + std::to_string(img[i]) + "u,";
    }
    src += "\n};\n";
    return src;
}

} // namespace

// The survivor_tail16 program and kernel. Returns the build error, or an empty
// string.
std::string SurvivorRunner::Setup::build_tail16(SurvivorRunner& runner, cl_device_id chosen_dev) {
    cl_int status = CL_SUCCESS;
    const std::string source =
        std::string(kernels::u128_limbs_h) + "\n" + std::string(kernels::survivor_tail16_cl);
    const char* src_ptr = source.c_str();
    const std::size_t src_len = source.size();
    runner.program_ = clCreateProgramWithSource(runner.context_, 1, &src_ptr, &src_len, &status);
    check(status, "clCreateProgramWithSource");

    status = clBuildProgram(runner.program_, 1, &chosen_dev, "-cl-std=CL1.2", nullptr, nullptr);
    if (status != CL_SUCCESS) {
        std::size_t log_size = 0;
        clGetProgramBuildInfo(
            runner.program_, chosen_dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        std::string log(log_size, '\0');
        clGetProgramBuildInfo(
            runner.program_, chosen_dev, CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
        return "clBuildProgram failed: " + cl_error_string(status) + "\n" + log;
    }

    runner.kernel_ = clCreateKernel(runner.program_, "survivor_tail16", &status);
    check(status, "clCreateKernel");
    return {};
}

// The tail_persistent build variant: the split sub-tables must fit the
// device's local memory, next to the kernel's own __local use.
void SurvivorRunner::Setup::tail_variant(SurvivorRunner& runner,
                                         cl_device_id chosen_dev,
                                         bool u64n) {
    runner.tail_u64n_ = u64n;
    runner.tail_desc_ =
        std::string("u64n=") + (u64n ? "1" : "0") + " split=" + std::to_string(kTailSplit);
    const unsigned bytes = 8u * u128q_split_entries();
    // A __local table larger than local memory fails at clCreateKernel, and
    // the run would fall back to the CPU without a reason, so refuse it here.
    // COLLATZ_GPU_LOCAL_MEM can only lower the device size (a test hook, like
    // COLLATZ_GPU_GLOBAL_MEM).
    cl_ulong lmem = dev_scalar<cl_ulong>(chosen_dev, CL_DEVICE_LOCAL_MEM_SIZE);
    if (const std::size_t forced = env_size_or("COLLATZ_GPU_LOCAL_MEM", 0);
        forced != 0 && forced < lmem) {
        lmem = forced;
    }
    if (bytes + 4096u > lmem) {
        throw std::runtime_error(
            "the split tail needs " + std::to_string(bytes) +
            " B of __local sub-tables (+ the kernel's own) but the device has " +
            std::to_string(static_cast<unsigned long long>(lmem)) + " B");
    }
}

void SurvivorRunner::Setup::create_cpi_program(SurvivorRunner& runner) {
    cl_int status = CL_SUCCESS;
    const std::string cpi_source = std::string(kernels::u128_limbs_h) + "\n" +
                                   split_table_source() + std::string(kernels::survivor_cpi_cl);
    const char* cpi_ptr = cpi_source.c_str();
    const std::size_t cpi_len = cpi_source.size();
    runner.program_cpi_ =
        clCreateProgramWithSource(runner.context_, 1, &cpi_ptr, &cpi_len, &status);
    check(status, "clCreateProgramWithSource(cpi)");
}

// Builds the cpi program and returns the build error, or an empty string.
std::string SurvivorRunner::Setup::build_cpi(SurvivorRunner& runner, cl_device_id chosen_dev) {
    cl_int status = CL_SUCCESS;
    // The cpi program takes the branch-free 3^f (u128_limbs.h) and the tail
    // option (tail_defaults.hpp).
    const std::string cpi_opts = "-cl-std=CL1.2 -DCOLLATZ_GPU_POW3_LOCAL=1" +
                                 tail_build_defines(runner.tail_u64n_) +
                                 (runner.sc_inject_dropwg_ ? " -DCOLLATZ_GPU_INJECT_DROPWG=1" : "");
    status =
        clBuildProgram(runner.program_cpi_, 1, &chosen_dev, cpi_opts.c_str(), nullptr, nullptr);
    if (status != CL_SUCCESS) {
        std::size_t log_size = 0;
        clGetProgramBuildInfo(
            runner.program_cpi_, chosen_dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        std::string log(log_size, '\0');
        clGetProgramBuildInfo(
            runner.program_cpi_, chosen_dev, CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
        return "clBuildProgram(cpi) failed: " + cl_error_string(status) + "\n" + log;
    }
    return {};
}

// The device mask and the persistent tail, the two kernels a run enqueues, and
// the largest work-group the mask can launch with. The mask runs 2^A/32
// work-items per group (512 at A=14), so verify_gpu compares A against it before 
// a run starts instead of failing at the first launch.
void SurvivorRunner::Setup::cpi_kernels(SurvivorRunner& runner, cl_device_id chosen_dev) {
    cl_int status = CL_SUCCESS;
    runner.kern_mask_ =
        clCreateKernel(runner.program_cpi_, "cpi_mask_scatter_coalesced_wgscan", &status);
    check(status, "clCreateKernel(cpi_mask_scatter_coalesced_wgscan)");
    runner.kern_persistent_ = clCreateKernel(runner.program_cpi_, "tail_persistent", &status);
    check(status, "clCreateKernel(tail_persistent)");
    std::size_t kernel_wg = 0;
    check(clGetKernelWorkGroupInfo(runner.kern_mask_,
                                   chosen_dev,
                                   CL_KERNEL_WORK_GROUP_SIZE,
                                   sizeof(kernel_wg),
                                   &kernel_wg,
                                   nullptr),
          "clGetKernelWorkGroupInfo(mask)");
    // One entry per dimension; the dimension count is the device's.
    std::size_t bytes = 0;
    check(clGetDeviceInfo(chosen_dev, CL_DEVICE_MAX_WORK_ITEM_SIZES, 0, nullptr, &bytes),
          "clGetDeviceInfo(CL_DEVICE_MAX_WORK_ITEM_SIZES)");
    std::vector<std::size_t> items(std::max<std::size_t>(1, bytes / sizeof(std::size_t)), 0);
    check(clGetDeviceInfo(chosen_dev,
                          CL_DEVICE_MAX_WORK_ITEM_SIZES,
                          items.size() * sizeof(std::size_t),
                          items.data(),
                          nullptr),
          "clGetDeviceInfo(CL_DEVICE_MAX_WORK_ITEM_SIZES)");
    runner.mask_local_max_ = std::min(kernel_wg, items[0]);
}

// The 16-step table.
void SurvivorRunner::Setup::tables(SurvivorRunner& runner) {
    cl_int status = CL_SUCCESS;
    const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16();
    runner.tbl16_buf_ = clCreateBuffer(runner.context_,
                                       CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       tbl.size() * sizeof(std::uint32_t),
                                       const_cast<std::uint32_t*>(tbl.data()),
                                       &status);
    check(status, "clCreateBuffer(tbl16)");
}

// The self-check test hooks and the in-flight pipeline's flag-list prefix.
void SurvivorRunner::Setup::dispatch_settings(SurvivorRunner& runner) {
    runner.sc_inject_expand_ = env_flag("COLLATZ_GPU_SELFCHECK_INJECT_EXPAND", false);
    runner.sc_inject_lost_ = env_flag("COLLATZ_GPU_SELFCHECK_INJECT_LOST", false);
    runner.cl_fail_at_ = env_size_or("COLLATZ_GPU_INJECT_CL_FAIL", 0);
    runner.pipe_.flag_k = kFlagReadK;
}

} // namespace collatz::gpu
