#ifndef COLLATZ_REF_REF_STEP3_HPP
#define COLLATZ_REF_REF_STEP3_HPP

// Reference "Step 3" trajectory finisher and result digest, written from the
// definitions and the interface docs in include/collatz/step3/; it shares no
// code with src/core/step3_*.cpp. All value arithmetic is GMP (mpz), recomputed
// per candidate; the digest mix is plain u64 because the digest is defined on
// 64-bit words.

#include "collatz/u128.hpp"

#include <cstdint>

namespace collatz::ref {

/// splitmix64 finalizer (the mix64 used by the pow result digest).
[[nodiscard]] std::uint64_t mix64(std::uint64_t z) noexcept;

/// The per-candidate pow digest term d(n, blocks, v_end):
///   key = lo64(n) ^ rotl64(hi64(n),29) ^ (blocks * M1)
///         ^ (lo64(v_end) * M2) ^ rotl64(hi64(v_end),43)
///   d   = mix64(key)
/// with M1 = 0xBF58476D1CE4E5B9, M2 = 0x94D049BB133111EB.
[[nodiscard]] std::uint64_t
result_digest_term(collatz::u128 n, std::uint64_t blocks, collatz::u128 v_end) noexcept;

/// Result of the 16-step block recurrence alone (no max walk): the block count
/// and pow digest inputs for one number.
struct BlockResult {
    std::uint64_t blocks = 0;   ///< 16-step blocks from m until first landing < n (0 for n <= 2).
    collatz::u128 v_end = 0;    ///< Landing value after the last block (n for n <= 2).
    bool overflow = false;      ///< True iff the landing exceeded u128 (unreachable for N <= 64).
    std::uint64_t pow_term = 0; ///< result_digest_term(n, blocks, v_end).
};

/// Runs the 16-step block recurrence from m (= T^k(n) >= n) until it first lands
/// strictly below n; used both for checked candidates (m = m_a) and for the
/// preflight numbers (m = n, the "verify one" computation).
[[nodiscard]] BlockResult finish_blocks(collatz::u128 n, collatz::u128 m);

/// Outcome of finishing one candidate.
struct Finish {
    /// Number of 16-step blocks executed until the block recurrence from m_a
    /// first lands strictly below n (summed into blocks_total). 0 for the n <= 2
    /// base case.
    std::uint64_t blocks = 0;
    /// The landing value after the last block (first block-boundary value < n);
    /// n itself for the n <= 2 base case.
    collatz::u128 v_end = 0;
    /// Exact maximum over the full trajectory from n (T applied from n) until it
    /// first drops strictly below n.
    collatz::u128 max_excursion = 0;
    /// True iff the exact maximum exceeded 2^128 - 1 (unreachable for N <= 64).
    bool max_exceeds_u128 = false;
    /// The pow digest term d(n, blocks, v_end) (result_digest_term).
    std::uint64_t pow_term = 0;
};

/// Finishes candidate (n, m_a) with m_a = T^{N-A}(n) >= n: runs the 16-step
/// block recurrence in GMP for `blocks`/`v_end`, walks the full trajectory from
/// n in GMP for the exact `max_excursion`, and computes the pow digest term.
[[nodiscard]] Finish finish(collatz::u128 n, collatz::u128 m_a);

} // namespace collatz::ref

#endif // COLLATZ_REF_REF_STEP3_HPP
