#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/collatz_fn.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "step1_gmp.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>

using collatz::u128;
using collatz::u128_max;

TEST_CASE("single-step T on small values") {
    CHECK(collatz::T(1).value == 2); // (3*1+1)/2
    CHECK(collatz::T(2).value == 1); // 2/2
    CHECK(collatz::T(3).value == 5); // (9+1)/2
    CHECK_FALSE(collatz::T(3).overflow);
}

TEST_CASE("overflow detection near 2^128") {
    // The largest odd value: 3n+1 overflows.
    bool expect_overflow = true;
    const auto r = collatz::T(u128_max);
    CHECK(r.overflow == expect_overflow);

    // n = 2^126 + 1 is odd, and 3n+1 = 2^127 + 2^126 + 4 < 2^128 fits.
    const u128 odd_safe = (static_cast<u128>(1) << 126) | 1;
    const auto r2 = collatz::T(odd_safe);
    CHECK_FALSE(r2.overflow);
}

TEST_CASE("small differential: k native T steps match GMP where no overflow") {
    // 1500 samples keep this unit test below 1 s even with ASan/UBSan; the
    // 10^6-seed sweep is in tests/integration/test_oracle.cpp.
    std::mt19937_64 rng(0xC011A72ULL);
    constexpr int kSamples = 1500;
    for (int i = 0; i < kSamples; ++i) {
        const std::uint64_t hi = rng();
        const std::uint64_t lo = rng();
        // Mix of full-width and reduced-width seeds to exercise both regimes.
        u128 n = collatz::make_u128(hi, lo);
        if ((i & 1) != 0) {
            n >>= (i % 96); // some smaller values that rarely overflow
        }
        if (n == 0) {
            n = 1;
        }
        const std::uint64_t k = 1 + (rng() % 12);
        u128 native = n;
        bool overflow = false;
        for (std::uint64_t j = 0; j < k && !overflow; ++j) {
            const collatz::StepResult s = collatz::T(native);
            overflow = s.overflow;
            native = s.value;
        }
        if (overflow) {
            continue; // GMP would exceed u128; nothing to compare
        }
        const mpz_class expected =
            collatz::test::iterate_T(collatz::oracle::to_mpz(n), static_cast<unsigned>(k));
        CHECK(collatz::oracle::to_mpz(native) == expected);
    }
}
