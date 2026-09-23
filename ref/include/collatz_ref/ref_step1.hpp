#ifndef COLLATZ_REF_REF_STEP1_HPP
#define COLLATZ_REF_REF_STEP1_HPP

// Slow reference implementation of the Angeltveit "Step 1" sieve
// classification (Descent / Path-Merging / OEE), written for clarity.
//
// It follows the mathematical definitions and shares no code with the
// verifier's implementation (include/collatz/step1.hpp, src/core/step1.cpp),
// so that tests/integration/test_ref_step1_survivors.cpp can check one against
// the other.
//
// All arithmetic is arbitrary-precision (GMP), recomputed per n0 without
// shortcuts. Total work is O(2^M * M), practical for M <= 26.

#include "collatz/u128.hpp"

#include <vector>

namespace collatz::ref {

/// Reason a start value n0 is excluded from being a "survivor" at depth M.
/// `none` means the trajectory survived every check up to depth M.
enum class Excl {
    none,       ///< Survivor: no exclusion fired through depth M.
    descent,    ///< T^k(n0) dropped strictly below n0.
    path_merge, ///< Path-merging: a smaller predecessor reaches the same node.
    oee,        ///< Odd-Even-Even merge (Hercher Lemma 9 style).
    mod3t,      ///< Generalized mod-3^t preimage-chain merge (collatz/preimage_chain.hpp).
};

/// Outcome of classifying a single start value: the reason and the depth k at
/// which it fired (for survivors, depth == M).
struct Classification {
    Excl by = Excl::none;
    unsigned depth = 0;
};

/// Which sieve rules are enabled. All on by default; tests enable subsets to
/// check each rule on its own.
struct Options {
    bool descent = true;
    bool path_merge = true;
    bool oee = true;
    /// Generalized mod-3^t preimage-chain merge. 0 = off; t >= 1 enables
    /// it after descent/path_merge/oee. This reference computes the rule
    /// directly on the concrete GMP value m = T^k(n0): it brute-forces the
    /// backward preimage tree of m to depth 2t, picks the minimal-ratio chain
    /// that lands on a smaller integer, and excludes the class when that chain
    /// satisfies f >= v, A(m) < n0, and 2^L*3^{f-v} <= 2^k. See
    /// collatz/preimage_chain.hpp for the derivation these conditions come from.
    unsigned mod3t = 0;
};

/// Classifies n0 in [0, 2^M) under the accelerated Collatz map
/// T(n) = n/2 (n even) or (3n+1)/2 (n odd), following the literal precedence:
///
///   * n0 even                 -> descent, depth 1 (every positive even halves)
///   * n0 == 1 (mod 4)         -> descent, depth 2 ((3n+1)/4 < n for n >= 5)
///   * otherwise iterate m_k = T^k(n0), and for k = 2..M in increasing order
///     return the first hit among, in this precedence:
///       (1) descent    : m_k < n0
///       (2) path_merge : m_k % 3 == 2 and (2*m_k - 1)/3 < n0
///       (3) oee        : some j in [2, k-3] with m_j..m_{k-3} all odd,
///                        m_{k-2} and m_{k-1} both even, and (m_j - 1)/2 < n0
///     if nothing fires through depth M -> Excl::none (a survivor).
[[nodiscard]] Classification classify(collatz::u128 n0, unsigned M, const Options& opts);

/// If classify(n0, M, opts) yields Excl::oee, sets out_j to the smallest valid
/// OEE witness index j (2 <= j <= depth-3) and out_k to the firing depth, and
/// returns true. Otherwise returns false and leaves the outputs unchanged.
///
/// The witness satisfies the Hercher identity (verified in tests):
///     T^{k-j}(m_j) == T^{k-j-1}((m_j - 1)/2)
/// i.e. the smaller number (m_j-1)/2 reaches the same node one step sooner.
[[nodiscard]] bool
oee_witness(collatz::u128 n0, unsigned M, const Options& opts, unsigned& out_j, unsigned& out_k);

/// A survivor: a start value with Excl::none, together with its terminal node
/// m = T^M(n0) and the odd-step count f = f_M (# of odd m_j for j in 0..M-1).
struct Survivor {
    collatz::u128 n0;
    collatz::u128 m;
    unsigned f;
};

/// All survivors in [0, 2^M), sorted ascending by n0 (natural iteration order).
/// The terminal node m and odd-step count f fit in u128/unsigned for the
/// supported range (M <= 26).
[[nodiscard]] std::vector<Survivor> survivors(unsigned M, const Options& opts);

/// Definitional high-bit partial-sieve threshold a* (the partial-OEE rule),
/// computed from the definition with GMP. With only opts.oee set it is the
/// independent counterpart of the a* that the Step-1 DFS carries
/// (collatz::step1::Node::oee_astar).
///
/// For each extension a in [0, 2^A) it forms the true candidate n = n0 + a*2^M,
/// walks its trajectory to depth M, and tests whether any enabled partial
/// certificate fires on that concrete value, restricted to arms/depths that
/// complete by depth M (so the parity pattern is class-invariant):
///   * opts.oee        : an odd m_j (j < M) followed by l >= 1 odd then 2 even
///                       steps all within [0, M), with (m_j - 1)/2 < n;
///   * opts.descent    : some depth k in [1, M] with m_k = T^k(n) < n;
///   * opts.path_merge : some depth k in [1, M] with m_k == 2 (mod 3) and
///                       (2 m_k - 1)/3 < n.
/// It returns the smallest a from which all larger a in [0, 2^A) are excluded,
/// or UINT32_MAX if no a in range is excluded. (The excluded set is an up-set,
/// so this is well defined.)
[[nodiscard]] unsigned
partial_oee_a_star(collatz::u128 n0, unsigned M, unsigned A, const Options& opts);

} // namespace collatz::ref

#endif // COLLATZ_REF_REF_STEP1_HPP
