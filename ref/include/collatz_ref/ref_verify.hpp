#ifndef COLLATZ_REF_REF_VERIFY_HPP
#define COLLATZ_REF_REF_VERIFY_HPP

// Reference end-to-end verifier: Step 1 (ref::survivors) -> Step 2
// (ref::candidates) -> Step 3 (ref::finish), aggregating the same result
// scalars as verify::Result. It shares no code with src/core/verify.cpp.
//
// Preflight convention (see the verify.hpp file comment). The verifier directly
// verifies every n < 2^17 with an exact walk and counts those "preflight numbers"
// in candidates_checked; the Step-2/Step-3 look-ahead is only sound for iterates
// >= 99781, so candidates with n < 2^17 are handled by the preflight and are
// not re-counted as look-ahead candidates. This reference mirrors that: it adds
// kPreflightCount to candidates_checked and skips any candidate with
// n < 2^17 from the Step-3 aggregates (blocks_total, pow_v4, max, argmax). The
// preflight count is a convention of run_preflight (src/core/verify_step1.cpp)
// that the interface docs do not state; tests/integration/test_ref_random.cpp
// checks it against verify::run.

#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"

#include <cstdint>

namespace collatz::ref {

/// Mersenne prime field used by the coverage and pow digest folds.
inline constexpr std::uint64_t kFieldP = (std::uint64_t{1} << 61) - 1;

/// Preflight boundary: every n < 2^17 is discharged by the exact preflight walk
/// (see the verify.hpp file comment).
inline constexpr collatz::u128 kPreflightBound = collatz::u128{1} << 17;

/// Number of "preflight numbers" the verifier folds into candidates_checked for
/// the n < 2^17 preflight; tests/integration/test_ref_random.cpp checks it
/// against verify::run.
inline constexpr std::uint64_t kPreflightCount = (std::uint64_t{1} << 17) - 1;

/// Aggregate result of a reference verification run: the field-for-field
/// analogue of the subset of verify::Result that the reference derives.
struct VerifyResult {
    std::uint64_t candidates_checked = 0; ///< kPreflightCount + checked look-ahead candidates.
    std::uint64_t checked_candidates = 0; ///< Look-ahead candidates only (n >= 2^17).
    std::uint64_t coverage = 0;           ///< Set-fold over every verified n mod 2^61-1.
    std::uint64_t pow_v4 = 0;             ///< Result digest mod 2^61-1 (pow_checksum).
    std::uint64_t blocks_total = 0;       ///< Wrapping u64 sum of 16-step block counts.
    collatz::u128 max_excursion = 0;      ///< Largest excursion over checked candidates.
    collatz::u128 max_excursion_n = 0;    ///< Candidate attaining it (smallest on ties).
    bool max_exceeds_u128 = false;
};

/// Runs the reference verifier for (N, A, B, num_bitvectors, partial_oee,
/// window_chains, window_templates_D) with the given Step-1 sieve options.
[[nodiscard]] VerifyResult verify_ref(unsigned N,
                                      unsigned A,
                                      unsigned B,
                                      unsigned num_bitvectors = 8,
                                      bool partial_oee = true,
                                      const Options& step1_opts = {},
                                      unsigned window_chains = 0,
                                      unsigned window_templates_D = 0);

} // namespace collatz::ref

#endif // COLLATZ_REF_REF_VERIFY_HPP
