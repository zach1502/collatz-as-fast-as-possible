#pragma once

// GMP helpers shared by the Step 3 unit tests (tests/core/test_step3_*.cpp).
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>

namespace collatz::test {

// Convert an mpz (known to fit) to u128 via its decimal string.
inline u128 mpz_to_u128(const mpz_class& x) {
    u128 out = 0;
    REQUIRE(collatz::from_string(collatz::oracle::to_decimal(x), out));
    return out;
}

// One accelerated T-step in GMP: x/2 (even) or (3x+1)/2 (odd).
inline void step_mpz(mpz_class& x) {
    if (mpz_even_p(x.get_mpz_t())) {
        x /= 2;
    } else {
        x = (3 * x + 1) / 2;
    }
}

} // namespace collatz::test
