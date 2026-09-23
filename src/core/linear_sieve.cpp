// The 2^k descent sieve: Sieve2k::build.

#include "collatz/collatz_fn.hpp"
#include "collatz/linear.hpp"
#include "collatz/u128.hpp"

#include <cstdint>
#include <vector>

namespace collatz::linear {

namespace {

/// Returns whether residue class r (mod 2^k) provably descends within k steps.
///
/// Theorem (Barina/Terras): if T^j(r) < r for some j <= k, then T^j(n) < n for
/// every n = r (mod 2^j) with n >= r. The class mod 2^k lies inside that class and r
/// is its smallest member, so the whole class descends. The two early returns
/// cover r = 0 (even) and r = 1 (1 mod 4), which do not descend themselves; every
/// other member of their classes does.
[[nodiscard]] bool residue_descends(std::uint64_t r, unsigned k) noexcept {
    if (k >= 1 && (r & 1u) == 0u) {
        return true; // every even residue halves to something smaller
    }
    if (k >= 2 && (r & 3u) == 1u) {
        return true; // r == 1 mod 4 descends within 2 steps (and covers r = 1)
    }
    const u128 start = static_cast<u128>(r);
    u128 v = start;
    for (unsigned j = 0; j < k; ++j) {
        // Bounded by r * (3/2)^k < 2^48 for k <= 30, so T never overflows here.
        v = T(v).value;
        if (v < start) {
            return true;
        }
    }
    return false;
}

} // namespace

void Sieve2k::build(unsigned k) {
    k_ = k;
    if (k == 0) {
        bitmap_.clear();
        return;
    }
    const std::uint64_t size = std::uint64_t{1} << k;
    bitmap_.assign(static_cast<std::size_t>((size + 63) / 64), 0);
    for (std::uint64_t r = 0; r < size; ++r) {
        if (residue_descends(r, k)) {
            bitmap_[static_cast<std::size_t>(r >> 6)] |= (std::uint64_t{1} << (r & 63u));
        }
    }
}

} // namespace collatz::linear
