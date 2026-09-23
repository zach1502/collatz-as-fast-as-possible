#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

// Differential tests over the whole u128 domain.
//
// Every native-width arithmetic path of the verifier runs on adversarial values
// across the u128 range and is compared with the GMP oracle and the ref/ GMP
// block recurrence. A fast u128 kernel that would pass 2^128 sets its overflow
// flag and the caller replays the walk in GMP, which is exact; without overflow
// the kernel matches the oracle bit for bit. Starting values go up to 2^127.
//
// Scope:
//   (1) block recurrence: finish_block16 vs the GMP block replays and ref/,
//       incl. the promotion boundary (v ~ 2^54), the block-landing guard
//       boundary (v where (v>>16)*3^f overflows u128), and n near / above 2^64
//       up to 2^127.
//   (2) the threshold-record upper bounds (v*3^16)>>16 and v<<15 must saturate
//       (set overflow), never wrap silently, for v near 2^128; the observable
//       exact-max contract is checked against the oracle.
//   (3) the GPU 4x32-limb recurrence (host-compiled) vs u128 vs GMP at the same
//       adversarial points, incl. the MAX_BLOCKS cap semantics.
//   (4) table16 agrees with GMP on 10^5 adversarial (n, m = T^k(n)) pairs
//       spanning 2^20 .. 2^127.
//   (5) u128 helpers (checked ops, to_string/from_string) at the domain edges.
//
// This file holds (2, direct) and (5). test_u128_domain_kernels.cpp, the other
// source of the same executable, holds (1), (2, observable), (3) and (4).
//
// All seeds are fixed.

#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "u128_domain.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <vector>

using collatz::u128;
using namespace collatz::test::u128_domain;

namespace {

// A random u128 with a random bit width in [0, 128], covering small values,
// limb boundaries, and near-max values uniformly across scale.
u128 rand_u128(std::mt19937_64& rng) {
    const int bits = static_cast<int>(rng() % 129);
    if (bits == 0) {
        return 0;
    }
    u128 x = collatz::make_u128(rng(), rng());
    if (bits < 128) {
        x &= (static_cast<u128>(1) << bits) - 1;
    }
    return x;
}

} // namespace

// (5) u128 helpers at the domain edges.
TEST_CASE("u128 checked arithmetic sets overflow at the 2^128 boundary") {
    bool ovf = false;
    // add: u128_max + 1 wraps to 0 and flags overflow.
    CHECK(collatz::checked_add(collatz::u128_max, 1, ovf) == 0);
    CHECK(ovf);
    ovf = false;
    CHECK(collatz::checked_add(collatz::u128_max, 0, ovf) == collatz::u128_max);
    CHECK_FALSE(ovf);
    // mul: exact product past 2^128 flags; 2^64 * 2^64 == 2^128 overflows.
    ovf = false;
    (void) collatz::checked_mul(static_cast<u128>(1) << 64, static_cast<u128>(1) << 64, ovf);
    CHECK(ovf);
    ovf = false;
    CHECK(collatz::checked_mul(collatz::u128_max, 1, ovf) == collatz::u128_max);
    CHECK_FALSE(ovf);
    ovf = false;
    (void) collatz::checked_mul(collatz::u128_max, 2, ovf);
    CHECK(ovf);
}

// (2, direct) The threshold-record upper bounds, (v * 3^16) >> 16 (block peak
// bound) and v << 15 (record pre-check), must be computed with the checked
// primitives so they saturate at 2^128 rather than wrap silently. The building
// blocks must flag overflow for v near the top of the domain.
TEST_CASE("threshold-record upper bounds saturate (no silent wrap) near 2^128") {
    const u128 p3_16 = collatz::pow3(16); // 43046721
    for (const u128 v : adversarial_points()) {
        bool ovf = false;
        const u128 prod = collatz::checked_mul(v + (v < collatz::u128_max ? 1 : 0), p3_16, ovf);
        // Mathematically (v+1) * 3^16 overflows u128 exactly when v is beyond
        // ~2^101.6; assert the flag agrees with GMP.
        const mpz_class exact = (u128_to_mpz(v) + 1) * u128_to_mpz(p3_16);
        const bool truly_overflows = mpz_sizeinbase(exact.get_mpz_t(), 2) > 128;
        CAPTURE(collatz::to_string(v));
        CHECK(ovf == truly_overflows);
        if (!ovf) {
            CHECK(prod == mpz_to_u128(exact));
        }
        // v << 15 (the record pre-check) must flag overflow past 2^113.
        bool sovf = false;
        const u128 sh = collatz::checked_mul(v, static_cast<u128>(1) << 15, sovf);
        const mpz_class shex = u128_to_mpz(v) << 15;
        const bool sh_ovf = mpz_sizeinbase(shex.get_mpz_t(), 2) > 128;
        CHECK(sovf == sh_ovf);
        if (!sovf) {
            CHECK(sh == mpz_to_u128(shex));
        }
    }
}

TEST_CASE("u128 to_string / from_string round-trip at the domain edges") {
    std::vector<u128> pts = adversarial_points();
    std::mt19937_64 rng(0xD00Du);
    for (int i = 0; i < 4000; ++i) {
        pts.push_back(rand_u128(rng));
    }
    for (const u128 x : pts) {
        const std::string dec = collatz::to_string(x);
        CHECK(dec == collatz::oracle::to_decimal(u128_to_mpz(x)));
        u128 back = 0;
        REQUIRE(collatz::from_string(dec, back));
        CHECK(back == x);
    }
    // Overflow / malformed input is rejected, not wrapped.
    u128 dummy = 123;
    CHECK_FALSE(collatz::from_string("340282366920938463463374607431768211456", dummy)); // 2^128
    CHECK(collatz::from_string("340282366920938463463374607431768211455", dummy));       // 2^128-1
    CHECK(dummy == collatz::u128_max);
    CHECK_FALSE(collatz::from_string("", dummy));
    CHECK_FALSE(collatz::from_string("12x3", dummy));
}
