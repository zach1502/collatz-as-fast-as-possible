#ifndef COLLATZ_GPU_VERIFY_GPU_INTERNAL_HPP
#define COLLATZ_GPU_VERIFY_GPU_INTERNAL_HPP

// Private declarations shared by the verify_gpu*.cpp files: the clock, the limb
// conversions, the coverage and digest terms, the prefix and tail bounds, and
// the pipeline constants.

#include "collatz/gpu/opencl.hpp"
#include "collatz/gpu/u128_limbs.h"
#include "collatz/u128.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace collatz::gpu::verify_gpu_detail {

using Clock = std::chrono::steady_clock;

inline constexpr std::uint64_t kCoveragePrime = 2305843009213693951ULL; // 2^61 - 1
inline constexpr unsigned kPreflightBits = 17;
inline constexpr collatz::u128 kPow3_16 = 43046721; // 3^16

// A u128 crosses to the device as four little-endian 32-bit limbs (s[0] = bits
// 0..31), the u128q layout of u128_limbs.h.
[[nodiscard]] inline cl_uint4 to_uint4(collatz::u128 x) {
    const std::uint64_t lo = collatz::lo64(x);
    const std::uint64_t hi = collatz::hi64(x);
    cl_uint4 v;
    v.s[0] = static_cast<cl_uint>(lo & 0xFFFFFFFFu);
    v.s[1] = static_cast<cl_uint>((lo >> 32) & 0xFFFFFFFFu);
    v.s[2] = static_cast<cl_uint>(hi & 0xFFFFFFFFu);
    v.s[3] = static_cast<cl_uint>((hi >> 32) & 0xFFFFFFFFu);
    return v;
}

[[nodiscard]] inline collatz::u128 from_uint4(const cl_uint4& v) {
    const std::uint64_t lo =
        static_cast<std::uint64_t>(v.s[0]) | (static_cast<std::uint64_t>(v.s[1]) << 32);
    const std::uint64_t hi =
        static_cast<std::uint64_t>(v.s[2]) | (static_cast<std::uint64_t>(v.s[3]) << 32);
    return collatz::make_u128(hi, lo);
}

// Coverage key of n. The host preflight, the GPU kernel and the CPU verifier
// all use the u128_limbs.h helpers, so their folds agree.
[[nodiscard]] inline std::uint64_t cov_key(collatz::u128 n) {
    return u128q_cov_key(u128q_from_halves(collatz::lo64(n), collatz::hi64(n)));
}

// Reduce any x < 2^128 modulo P = 2^61-1 (same as verify.cpp mod_mersenne61_u128).
// 2^61 == 1 (mod P), so x == (x & P) + ((x >> 61) & P) + (x >> 122) (mod P); the
// three limbs sum to < 2^62, and one more Mersenne fold lands in [0, P). The CPU
// fallback sums coverage and pow digest terms raw in a u128 and reduces once at
// merge. Reduction mod P is a ring homomorphism, so that equals the device's
// per-term fold and saves two reductions per candidate.
[[nodiscard]] inline std::uint64_t mod_mersenne61_u128(collatz::u128 x) noexcept {
    const std::uint64_t p = kCoveragePrime;
    const std::uint64_t s = (collatz::lo64(x) & p) + (static_cast<std::uint64_t>(x >> 61) & p) +
                            static_cast<std::uint64_t>(x >> 122);
    return static_cast<std::uint64_t>(u128q_mod_mersenne61(s));
}

// Cheap prefix upper bound pmax + smax*a (== verify_fold.hpp prefix_ub).
[[nodiscard]] inline collatz::u128
prefix_ub(collatz::u128 pmax, collatz::u128 smax, unsigned a) noexcept {
    bool ovf = false;
    const collatz::u128 term = collatz::checked_mul(smax, static_cast<collatz::u128>(a), ovf);
    if (ovf) {
        return collatz::u128_max;
    }
    const collatz::u128 ub = collatz::checked_add(pmax, term, ovf);
    return ovf ? collatz::u128_max : ub;
}

// Excursion upper bound from a max block-start value: (v * 3^16) >> 16.
[[nodiscard]] inline collatz::u128 tail_ub(collatz::u128 v) noexcept {
    bool ovf = false;
    const collatz::u128 prod = collatz::checked_mul(v, kPow3_16, ovf);
    return ovf ? collatz::u128_max : (prod >> 16);
}

inline constexpr std::size_t kMaxInFlight = 3; // cap resident batches (tens of MB each)
inline constexpr std::size_t kJobPoolCap = 2;
// Re-runs of a dispatch that fails its self-check before its batch is computed
// on the CPU.
inline constexpr int kScReruns = 3;

// Per-candidate pow digest term: the device's u128q_result_digest_term, which
// equals the CPU verifier's result_digest_term.
[[nodiscard]] inline std::uint64_t
digest_term(collatz::u128 n, std::uint64_t blocks, collatz::u128 vend) noexcept {
    return u128q_result_digest_term(u128q_from_halves(collatz::lo64(n), collatz::hi64(n)),
                                    blocks,
                                    u128q_from_halves(collatz::lo64(vend), collatz::hi64(vend)));
}

} // namespace collatz::gpu::verify_gpu_detail

#endif // COLLATZ_GPU_VERIFY_GPU_INTERNAL_HPP
