#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_HPP

// Host runtime for the survivor kernels. Step 1 runs on the CPU, and only the
// survivors, not their 2^A candidates, cross the bus: the device runs Step 2
// (the candidate mask) and Step 3 (the block recurrence).
//
// SurvivorRunner is the OpenCL driver: device pick, program build, table and
// bitvector upload, and batched dispatch with kernel and transfer timing.
// verify_gpu() runs Step 1 and the preflight on the CPU, streams the survivors
// through the kernels, folds the thread-count-independent checksums from the
// device outputs, and re-walks the rare flagged record contenders on the CPU
// for the exact maximum excursion. It falls back to the CPU verifier when no
// OpenCL GPU is usable.

#include "collatz/gpu/opencl.hpp"
#include "collatz/verify.hpp"
#include "opencl_runtime.hpp" // DeviceInfo, OpenCLError, cl_error_string
#include "survivor_runtime/batch_types.hpp"
#include "survivor_runtime/runner.hpp"
#include "survivor_runtime/runner_types.hpp"
#include "tail_defaults.hpp" // TailHint

#include <string>

namespace collatz::gpu {

/// GPU verifier: Step 1 and the preflight on the CPU, Steps 2 and 3 for every
/// survivor on the GPU, and a CPU re-walk of flagged contenders for the exact
/// maximum. Falls back to collatz::verify::run, setting *used_gpu = false, when
/// no OpenCL GPU is usable. The Result is bit-identical to
/// collatz::verify::run(opts) for the same options (N <= 64). *msg, when
/// non-null, gets a message on fallback or for diagnostics. A failure after the
/// device is set up (an OpenCL error during the run) throws std::runtime_error,
/// and there is no Result.
[[nodiscard]] collatz::verify::Result
verify_gpu(const collatz::verify::Options& opts, bool* used_gpu, std::string* msg);

} // namespace collatz::gpu

#include "survivor_runtime/runner_inline.hpp"

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_HPP
