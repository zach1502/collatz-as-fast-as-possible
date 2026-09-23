#pragma once

// The fixed tail_persistent settings. None of them changes a result.
//
//   COLLATZ_GPU_TAIL_U64N=1    the candidate n is one ulong (only when every
//                              n < 2^64, see tail_u64n_on)
//
// Each 16-step block's {3^f, c} is composed from kTailSplit-step sub-tables
// staged in __local. The grid has kPersistMult work-groups per compute unit,
// and a lane claims kPersistChunk candidates at a time.

#include <cstddef>
#include <string>

namespace collatz::gpu {

/// What verify_gpu knows about the run before SurvivorRunner::create builds the
/// cpi program. value_bits is the bit length of the largest checked n
/// (verify::value_bits: N for a full run, more for a frontier block).
struct TailHint {
    unsigned value_bits = 0;
};

/// Width of the slot loop's sub-tables; the kernel is written for 8.
inline constexpr unsigned kTailSplit = 8;
inline constexpr std::size_t kPersistMult = 24;
inline constexpr std::size_t kPersistChunk = 16;

// The cpi program's tail build option.
[[nodiscard]] inline std::string tail_build_defines(bool u64n) {
    return std::string(" -DCOLLATZ_GPU_TAIL_U64N=") + (u64n ? "1" : "0");
}

/// U64N forms n in one ulong, so it is on only when every n < 2^64.
[[nodiscard]] constexpr bool tail_u64n_on(const TailHint& hint) noexcept {
    return hint.value_bits <= 64;
}

} // namespace collatz::gpu
