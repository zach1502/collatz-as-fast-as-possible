#ifndef COLLATZ_GPU_BV_SPLIT_HPP
#define COLLATZ_GPU_BV_SPLIT_HPP

// Plans the split of the flat Step-2 bitvector table across up to 4 device
// buffers so no single allocation exceeds CL_DEVICE_MAX_MEM_ALLOC_SIZE. Pure
// arithmetic with no OpenCL calls, so it is unit-testable without a device.

#include <cstdint>

namespace collatz::gpu {

struct BvSplitPlan {
    bool ok = false;                ///< False => even one level exceeds max_alloc, or > 4 buffers.
    int num_buffers = 1;            ///< 1..4 live device buffers.
    unsigned levels_per_buffer = 8; ///< lpb in {8,4,2,1}: power-of-two level run per buffer.
    unsigned buf_words_log2 = 0;    ///< log2 of uint32 words per buffer (kernel bv_lg arg).
};

/// Plans the buffer split for a table of `nbv` levels, each 2^B/8 bytes (2^(B-5)
/// uint32 words), against a per-allocation cap `max_alloc`. Picks the largest
/// power-of-two levels-per-buffer whose bytes fit; `num_buffers == 1` reproduces
/// the single-buffer upload bit for bit (buf_words_log2 large enough that every
/// word maps to buffer 0).
[[nodiscard]] inline BvSplitPlan plan_bv_split(unsigned B, unsigned nbv, std::uint64_t max_alloc) {
    BvSplitPlan plan;
    if (B < 5 || nbv == 0) {
        return plan;
    }
    const std::uint64_t per_level_bytes = (static_cast<std::uint64_t>(1) << B) / 8u;
    int lpb_log2 = -1;
    for (int lg = 3; lg >= 0; --lg) {
        if ((static_cast<std::uint64_t>(1) << lg) * per_level_bytes <= max_alloc) {
            lpb_log2 = lg;
            break;
        }
    }
    if (lpb_log2 < 0) {
        return plan; // one level does not fit: caller keeps the CPU fallback.
    }
    const unsigned lpb = 1u << static_cast<unsigned>(lpb_log2);
    const int num_buffers = static_cast<int>((nbv + lpb - 1u) / lpb);
    if (num_buffers > 4) {
        return plan;
    }
    plan.ok = true;
    plan.num_buffers = num_buffers;
    plan.levels_per_buffer = lpb;
    plan.buf_words_log2 = static_cast<unsigned>(lpb_log2) + (B - 5u);
    return plan;
}

} // namespace collatz::gpu

#endif // COLLATZ_GPU_BV_SPLIT_HPP
