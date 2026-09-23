#ifndef COLLATZ_GPU_VERIFY_GPU_KNOBS_HPP
#define COLLATZ_GPU_VERIFY_GPU_KNOBS_HPP

// The run settings that GpuRunState's initializers and the dispatch code read:
// the batch and flag-list caps, the record list, the scalar kernel arguments and
// the host copy of the 16-step table.

#include "collatz/gpu/opencl.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "opencl_runtime.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/internal.hpp"
#include "verify_gpu/records.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace collatz::gpu::verify_gpu_detail {

// Largest survivor batch sent to the device in one call, default 2^20; a run
// at A > 12 caps it further at 2^(32-A) (batch_survivor_cap in GpuRunState).
// COLLATZ_GPU_FLUSH_CAP (>= 1) lets tests force many small batches at small N,
// where the run would otherwise be a single batch.
[[nodiscard]] inline std::size_t gpu_flush_cap() {
    // Read on every call, not cached in a static, so a test can set the cap for
    // its own verify_gpu run without leaking it into later runs.
    if (const char* e = std::getenv("COLLATZ_GPU_FLUSH_CAP")) {
        const long v = std::atol(e);
        if (v >= 1) {
            return static_cast<std::size_t>(v);
        }
    }
    return static_cast<std::size_t>(1u << 20);
}

// Work-group size of the candidate-per-item pass-2 kernel, a power of two for
// the tree reduction. Small groups keep more SIMD groups resident and shorten
// the reduction after the divergent tails finish.
inline constexpr std::size_t kCpiWg = 64;

// Capacity of a batch's flagged-candidate list. On overflow the batch falls
// back to the per-survivor kernel.
inline constexpr std::uint32_t kCpiFlagCap = 1u << 16;

// Record list (Options::record_threshold), as in verify::run: every flag
// bound and record gate is capped at bar_cap = X + 1, so each walked
// candidate whose excursion exceeds X is re-walked exactly and listed.
// Excursions above 2^128-1 only come from the GMP replays, so a threshold
// past u128 needs no cap.
[[nodiscard]] inline RecordList record_list(const collatz::verify::Options& opts) {
    RecordList rl;
    if (!opts.record_threshold.empty()) {
        rl.on = true;
        rl.dec = opts.record_threshold;
        rl.fits = collatz::from_string(rl.dec, rl.x);
        rl.cap = opts.records_cap;
    }
    return rl;
}

// The level upload writes the filter's u64 words as they are, which on a
// little-endian host are the flat uint32 table the kernels read.
static_assert(std::endian::native == std::endian::little,
              "the Step-2 bitvector upload needs a little-endian host");

// Scalar (whole-run-constant) kernel arguments.
[[nodiscard]] inline SurvivorConst kernel_const(const collatz::verify::Options& opts,
                                                unsigned M,
                                                cl_uint twoA,
                                                const collatz::step2::Filter& filter,
                                                collatz::u128 preflimit) {
    SurvivorConst kc;
    kc.b_mask = static_cast<cl_uint>((static_cast<std::uint64_t>(1) << opts.B) - 1);
    kc.twoA = twoA;
    kc.m_shift = M;
    kc.pow2m_mod9 = filter.pow2m_mod9();
    kc.preflimit = to_uint4(preflimit);
    kc.a_bits = opts.A; // candidate id packing shift: (s << A) | a
    return kc;
}

// Host copy of the 16-step table, which replay hands to the device code to ask
// whether the device folds a candidate. Static storage: 256 KiB would otherwise
// sit on the stack for the whole run.
[[nodiscard]] inline const std::array<std::uint32_t, 1u << 16>& host_tbl16() {
    static const std::array<std::uint32_t, 1u << 16> tbl16_host = build_tbl16();
    return tbl16_host;
}

} // namespace collatz::gpu::verify_gpu_detail

#endif // COLLATZ_GPU_VERIFY_GPU_KNOBS_HPP
