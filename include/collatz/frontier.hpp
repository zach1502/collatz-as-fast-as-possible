#ifndef COLLATZ_FRONTIER_HPP
#define COLLATZ_FRONTIER_HPP

// Frontier block mode: the survivor lift.
//
// A block [L, L + 2^K) with L = h * 2^K reuses the full run's Step-1 tree. For
// n = L + a*2^M + n0 (M = K - A, a < 2^A, n0 < 2^M) the low M bits of n are n0,
// so the parity word of the first M steps, and with it every Step-1
// certificate, is the one of n0; by the affine lemma
// T^M(n) = m + 3^f (a + h*2^A). Lifting a survivor replaces its class
// representative n0 by n0 + L, after which extension a of the lifted node is
// the block member L + a*2^M + n0, and every value-dependent Step-2 decision
// (mod-9 row from n0, window start j0 from m mod 2^B, partial-OEE a*) and the
// Step-3 start value read the block member, not the full-run residue.

#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

namespace collatz::frontier {

struct Lift {
    u128 L = 0;   ///< Block start h * 2^K.
    u128 h2A = 0; ///< h * 2^A: the block's offset in units of the extension a.
};

/// Lift parameters for block h at depth K with A high bits. The caller checks
/// that (h + 1) * 2^K fits (verify::validate_block).
[[nodiscard]] Lift make_lift(u128 h, unsigned K, unsigned A) noexcept;

/// Lifts one Step-1 survivor into the block:
///   n0' = n0 + L, m' = m + 3^f * h2A, pmax' = pmax + smax * h2A,
///   a*' = max(0, a* - h2A) for a real partial-OEE threshold; the
///   kOeeAStarNone sentinel is kept.
/// a*' is exact: extension a' of the lifted node is extension a' + h2A of the
/// unlifted one, excluded iff (a' + h2A) C > D, i.e. a' C > D - h2A C. f and smax
/// are unchanged. verify::validate_block bounds every lifted value below 2^125.
void lift_survivor(step1::Node& node, const Lift& lift) noexcept;

} // namespace collatz::frontier

#endif // COLLATZ_FRONTIER_HPP
