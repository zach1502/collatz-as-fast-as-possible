#ifndef COLLATZ_COLLATZ_FN_HPP
#define COLLATZ_COLLATZ_FN_HPP

// The accelerated Collatz map T over u128, with explicit overflow reporting.
// The GMP oracle (collatz/oracle.hpp) is the arbitrary-precision reference it
// is checked against.

#include "collatz/u128.hpp"

namespace collatz {

/// One map step: the next value and an overflow flag. When `overflow` is set,
/// `value` is the result wrapped mod 2^128, not the true value.
struct StepResult {
    u128 value = 0;
    bool overflow = false;
};

/// Accelerated Collatz map T(n): n/2 for even n, (3n+1)/2 for odd n. The odd
/// branch is exact because 3n+1 is even when n is odd. Overflow is reported if
/// 3n+1 exceeds 2^128 - 1 (the division cannot overflow).
[[nodiscard]] constexpr StepResult T(u128 n) noexcept {
    if ((n & 1) == 0) {
        return {n >> 1, false};
    }
    bool ovf = false;
    u128 t = checked_mul(n, static_cast<u128>(3), ovf);
    bool ovf2 = false;
    t = checked_add(t, static_cast<u128>(1), ovf2);
    return {t >> 1, ovf || ovf2};
}

} // namespace collatz

#endif // COLLATZ_COLLATZ_FN_HPP
