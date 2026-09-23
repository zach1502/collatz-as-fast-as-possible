#ifndef COLLATZ_TESTS_U128_DOMAIN_HPP
#define COLLATZ_TESTS_U128_DOMAIN_HPP

// The u128 / mpz_class conversions and the adversarial u128 points shared by
// tests/integration/test_u128_domain.cpp and test_u128_domain_kernels.cpp.

#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>
#include <vector>

namespace collatz::test::u128_domain {

using collatz::u128;

inline u128 mpz_to_u128(const mpz_class& x) {
    u128 out = 0;
    REQUIRE(collatz::from_string(collatz::oracle::to_decimal(x), out));
    return out;
}

inline mpz_class u128_to_mpz(u128 x) {
    return collatz::oracle::to_mpz(x);
}

// The adversarial fixed points: every representation/algorithm boundary the
// code special-cases, plus their immediate neighbours.
inline std::vector<u128> adversarial_points() {
    std::vector<u128> v;
    const auto add = [&](u128 x) { v.push_back(x); };
    const auto add_around = [&](u128 x) {
        if (x >= 2) {
            add(x - 2);
        }
        if (x >= 1) {
            add(x - 1);
        }
        add(x);
        add(x + 1);
        add(x + 2);
    };
    add(0);
    add(1);
    add(2);
    add(3);
    // u64 fast-lane promotion boundary (kU64FastBar = 2^54) and its shifts.
    add_around(static_cast<u128>(1) << 54);
    add_around(static_cast<u128>(1) << 38); // (v>>16) fast-lane multiplier bound
    // 64-bit limb boundary (candidate n beyond 2^64).
    add_around(static_cast<u128>(1) << 63);
    add_around(static_cast<u128>(1) << 64);
    // block-landing guard: hi64(v>>16) ~ 2^37 => v ~ 2^117; and the point where
    // (v>>16)*3^16 first reaches 2^128 (v ~ 2^102).
    add_around(static_cast<u128>(1) << 102);
    add_around(static_cast<u128>(1) << 117);
    // top of the domain.
    add_around(static_cast<u128>(1) << 96);
    add_around(static_cast<u128>(1) << 120);
    add((static_cast<u128>(1) << 127) - 1);
    add(static_cast<u128>(1) << 127);
    add(collatz::u128_max - 1);
    add(collatz::u128_max);
    return v;
}

} // namespace collatz::test::u128_domain

#endif // COLLATZ_TESTS_U128_DOMAIN_HPP
