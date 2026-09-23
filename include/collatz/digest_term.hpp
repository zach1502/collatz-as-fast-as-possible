#ifndef COLLATZ_DIGEST_TERM_HPP
#define COLLATZ_DIGEST_TERM_HPP

// Per-candidate term of the version-5 pow_checksum digest. The pass-2 fold in
// verify_flush.cpp and the inline fold in step3_persist.hpp both call it, so
// their summed digests are bit-identical. verify_internal.hpp re-exports these
// with using-declarations; certify_internal.hpp has an independent copy.

#include "collatz/u128.hpp"

#include <cstdint>

namespace collatz::digest {

/// splitmix64 finalizer.
[[nodiscard]] inline std::uint64_t mix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// Rotate-left a 64-bit word; r must be in [1,63], since a shift by 64 is undefined.
[[nodiscard]] inline std::uint64_t rotl64(std::uint64_t x, unsigned r) noexcept {
    return (x << r) | (x >> (64u - r));
}

/// Per-candidate term of the version-5 result digest (pow_checksum). Inputs: a
/// checked candidate n, the number of blocks it executed, and v_end (the
/// landing value after its last block: the first block-boundary value < n; n
/// itself for the n <= 2 base case). One splitmix64 finalizer over a 64-bit
/// combination of the triple binds them, so any single wrong triple changes the
/// term (and hence the summed digest mod 2^61-1) with probability ~1-2^-61, and
/// so does a pair of compensating +1/-1 block errors, which a plain block-count
/// sum cannot detect. Bit-identical to the OpenCL device term (u128_limbs.h /
/// the tail kernels).
[[nodiscard]] inline std::uint64_t
result_digest_term(u128 n, std::uint64_t blocks, u128 v_end) noexcept {
    const std::uint64_t key = lo64(n) ^ rotl64(hi64(n), 29) ^ (blocks * 0xBF58476D1CE4E5B9ULL) ^
                              (lo64(v_end) * 0x94D049BB133111EBULL) ^ rotl64(hi64(v_end), 43);
    return mix64(key);
}

} // namespace collatz::digest

#endif // COLLATZ_DIGEST_TERM_HPP
