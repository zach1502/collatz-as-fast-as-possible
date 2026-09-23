#ifndef COLLATZ_STEP3_EXACT_HPP
#define COLLATZ_STEP3_EXACT_HPP

// Step 3 exact GMP finisher (collatz/step3.hpp includes this file).
//
// finish_oracle is an arbitrary-precision GMP walk: exact, it never overflows,
// and it reports the maximum value seen along the way and whether the true
// excursion exceeds 2^128.
//
// It takes (n, m) with the precondition m = T^k(n) and m >= n, and walks from
// m (not from the original n). Passing m == n finishes the whole trajectory of
// n from the start, which is the Barina "verify one" computation
// (collatz::linear::verify_one_detailed).
//
// first_drop_peak computes the same maximum from n in u128 and falls back to
// finish_oracle(n, n) when a step leaves u128.

#include "collatz/u128.hpp"

#include <string>

namespace collatz::step3 {

/// Result of finishing a trajectory from m down below n.
struct Outcome {
    /// Maximum value seen on the finished trajectory (T-domain values). Exact.
    u128 max_seen = 0;
    /// True iff the true max excursion exceeds 2^128 - 1, in which case
    /// `max_seen` is saturated to u128_max.
    bool max_exceeds_u128 = false;
};

/// Finish via the arbitrary-precision GMP oracle. Exact and overflow-free; used
/// where a u128 walk could overflow. If the true excursion exceeds 2^128 - 1,
/// `max_exceeds_u128` is set and `max_seen` is saturated to u128_max.
/// `max_dec`, when non-null, receives the exact maximum in decimal if it
/// exceeds 2^128 - 1 and is cleared otherwise.
[[nodiscard]] Outcome finish_oracle(u128 n, u128 m, std::string* max_dec = nullptr);

/// The excursion of n that the verifier records: the largest T^j(n) for
/// 0 <= j < s, where s >= 1 is the first step with T^s(n) < n (n itself for
/// n <= 1). The block finishers can pass s (they test "< n" only at block
/// ends), so their max_seen bounds this value from above. Walks in u128; when
/// a step leaves u128 the walk from n is replayed by finish_oracle(n, n), so
/// the result is exact for every n. `max_exceeds_u128` and `max_dec` as in
/// finish_oracle.
///
/// `max_steps` stops a u128 walk that has not dropped after that many steps
/// (the prefix of a candidate). The result is then the largest of T^0(n) ..
/// T^max_steps(n), a lower bound on the excursion that equals it when no later
/// value before the drop is larger. A walk that drops first, or that leaves
/// u128, returns the whole excursion.
[[nodiscard]] Outcome
first_drop_peak(u128 n, std::string* max_dec = nullptr, unsigned max_steps = ~0u);

} // namespace collatz::step3

#endif // COLLATZ_STEP3_EXACT_HPP
