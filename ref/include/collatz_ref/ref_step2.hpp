#ifndef COLLATZ_REF_REF_STEP2_HPP
#define COLLATZ_REF_REF_STEP2_HPP

// Slow reference implementation of the Angeltveit "Step 2" look-ahead sieve,
// written from the definitions and the interface docs in
// include/collatz/step2.hpp. It shares no code with src/core/step2.cpp, so that
// tests/integration/test_ref_random.cpp can check the verifier's filter against
// it end to end.
//
// The dip function and the pruning rule follow the file comment of
// include/collatz/step2.hpp (Theorem 4.4):
//
//   dip_B(m) = max S_B(m), where S_B(m) collects, over the B-step parity
//   sequence of m (which depends only on m mod 2^B):
//     (A) descent    A_k(m)       = 306k - 485 f_k(m)          (0 <= k <= B)
//     (B) path-merge A_k(m) + 179 = 306(k-1) - 485(f_k(m)-1)   when T^k(m) == 2
//                    (mod 3) by the parity-only criterion (Lemma 2.8) with
//                    f_k >= 1.
//     (C) OEE        A_k(m) + 306 = 306(k+1) - 485 f_k(m)      when T^k(m)
//                    begins l >= 1 odd steps then 2 even steps, all inside the
//                    B-step window.
//
//   A survivor (n0, m, f) extension a in [0, 2^A) -- candidate n = n0 + a*2^{N-A}
//   with m_a = m + 3^f a and the same f -- is pruned iff any of:
//     * n mod 9 in {2,4,5,8}                        (mod-9 preimage sieve), or
//     * dip_B(m_a mod 2^B) >= 485 f - 306 (N-A) and f < fmin+num_bitvectors
//                                                   (look-ahead bitvector), or
//     * a >= a_star (high-bit partial OEE; only when partial_oee is on).
//   fmin = ceil((306(N-A)+1)/485). f < fmin makes the threshold <= 0, so the
//   bitvector clause prunes those extensions automatically.

#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"

#include <cstdint>
#include <vector>

namespace collatz::ref {

/// dip_B(m): max of S_B(m) as defined above. `m` is taken mod 2^B. Computed on
/// the actual (small) integer value of the residue; requires 2 <= B <= 32 so
/// the B-step iterate stays below 3^32 < 2^51 (exact in 64-bit). The
/// independent counterpart of collatz::step2::Filter::dip.
///
/// Window chain-merge terms: when `window_chains` (= t_w) is > 0, the dip
/// additionally considers, at every window step k with value v_k = T^k(m) and
/// f_k odd steps, the best admissible decreasing backward preimage chain of
/// v_k's class (odd-step count v <= min(f_k, t_w), explored to depth 2*t_w). Its
/// contraction margin is A_k + (485 v - 306 L). This is the counterpart of
/// preimage::WindowChainTable, computed on the concrete iterate v_k.
/// t_w = 0 leaves only the descent, path-merge and OEE terms.
///
/// General merge templates: when `window_templates_D` (= D) is > 0, the dip
/// also considers, at every window step, a deeper admissible backward preimage
/// chain of v_k's class (odd-step count v <= min(f_k, min(D, 12)), explored to
/// depth D). Every general (u, w) merge template reduces to such an empty-w
/// ancestor chain of odd depth <= D applied at a window step, so this deeper
/// walk covers the template family. The max over both walks keeps dip monotone
/// in D; D = 0 leaves only the t_w chain terms.
[[nodiscard]] std::uint32_t
dip_B(std::uint64_t m, unsigned B, unsigned window_chains = 0, unsigned window_templates_D = 0);

/// Parity-only criterion for "T^k(x) == 2 (mod 3)" (Lemma 2.8): given the parity
/// sequence `par` (par[s] = 1 iff step s took the odd branch) and a depth k,
/// returns true iff there is at least one odd step in [0, k) and an even number
/// of even steps have elapsed since the last odd step.
[[nodiscard]] bool parity_mod3_is_2(const std::vector<unsigned char>& par, unsigned k);

/// Smallest f with a positive look-ahead threshold: ceil((306(N-A)+1)/485).
[[nodiscard]] unsigned fmin_for(unsigned N, unsigned A);

/// True iff the extension a of the survivor (n0, m, f) must still be checked,
/// i.e. it survives the mod-9, look-ahead bitvector, and (when `partial_oee`)
/// high-bit partial-OEE sieves. `a_star` is the precomputed partial-OEE
/// threshold for the class (ref::partial_oee_a_star), passed in so the O(2^A M)
/// walk is done once per survivor rather than once per extension; it is ignored
/// unless `partial_oee` is true.
[[nodiscard]] bool must_check(collatz::u128 n0,
                              collatz::u128 m,
                              unsigned f,
                              unsigned a,
                              unsigned N,
                              unsigned A,
                              unsigned B,
                              unsigned num_bitvectors,
                              bool partial_oee,
                              unsigned a_star,
                              unsigned window_chains = 0,
                              unsigned window_templates_D = 0);

/// A surviving Step-2 candidate: the concrete value n = n0 + a*2^{N-A}, its
/// post-Step-1 value m_a = m + 3^f a, and the shared odd-step count f.
struct Candidate {
    collatz::u128 n;
    collatz::u128 m_a;
    unsigned f;
};

/// The full sorted (ascending by n) candidate list produced by Step 1 (mod-3^t
/// / OEE / etc. per `step1_opts`) followed by the Step-2 sieve above: the
/// independent counterpart of the verifier's checked-candidate set for a given
/// (N, A, B, num_bitvectors, partial_oee, step1 config).
[[nodiscard]] std::vector<Candidate> candidates(unsigned N,
                                                unsigned A,
                                                unsigned B,
                                                unsigned num_bitvectors,
                                                bool partial_oee,
                                                const Options& step1_opts,
                                                unsigned window_chains = 0,
                                                unsigned window_templates_D = 0);

} // namespace collatz::ref

#endif // COLLATZ_REF_REF_STEP2_HPP
