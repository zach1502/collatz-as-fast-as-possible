#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/collatz_fn.hpp"
#include "collatz/linear.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>

using collatz::u128;
namespace lin = collatz::linear;

namespace {

// Full sweep in optimized builds; a smaller sweep under sanitizers so the debug
// run stays fast.
#ifdef NDEBUG
constexpr int kSeeds = 2000;
#else
constexpr int kSeeds = 400;
#endif

/// Independent reference for the excursion: iterate the accelerated map T from
/// n0, tracking the max value seen, until it first drops strictly below n0.
/// verify_one_detailed must match it.
u128 reference_max_below(u128 n0) {
    if (n0 <= 1) {
        return n0;
    }
    u128 v = n0;
    u128 m = n0;
    for (;;) {
        v = collatz::T(v).value; // n0 < 2^60 in tests => no overflow
        if (v > m) {
            m = v;
        }
        if (v < n0) {
            break;
        }
    }
    return m;
}

/// Brute-force restatement of the descent-sieve predicate, written differently
/// from the library (collatz_fn T and an explicit representative walk) so it is
/// an independent oracle.
bool brute_skip(std::uint64_t r, unsigned k) {
    if (k >= 1 && (r % 2) == 0) {
        return true;
    }
    if (k >= 2 && (r % 4) == 1) {
        return true;
    }
    u128 v = static_cast<u128>(r);
    for (unsigned j = 0; j < k; ++j) {
        v = collatz::T(v).value;
        if (v < static_cast<u128>(r)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("verify_one_detailed matches the GMP oracle and the T-excursion reference") {
    std::mt19937_64 rng(0x11EA12ABCDULL);
    constexpr u128 mask60 = (static_cast<u128>(1) << 60) - 1;

    for (int i = 0; i < kSeeds; ++i) {
        u128 n0 = collatz::make_u128(rng(), rng()) & mask60;
        if (n0 < 2) {
            n0 = 2;
        }

        const lin::OneResult fast = lin::verify_one_detailed(n0);
        const std::uint64_t fast_cs = fast.checksum;
        const u128 fast_max = fast.max_seen;

        mpz_class oracle_max;
        const std::uint64_t oracle_cs = lin::verify_one_oracle(n0, oracle_max);

        // Fast kernel agrees with the arbitrary-precision oracle exactly.
        REQUIRE(fast_cs == oracle_cs);
        REQUIRE(collatz::oracle::to_mpz(fast_max) == oracle_max);

        // The excursion equals the independent T-map reference.
        REQUIRE(fast_max == reference_max_below(n0));
    }
}

TEST_CASE("checksum is independent of thread count") {
    const u128 hi = static_cast<u128>(1) << 16;
    lin::Options o1;
    o1.threads = 1;
    o1.sieve_bits = 0;
    o1.mod9 = false;
    lin::Options o4 = o1;
    o4.threads = 4;

    const lin::Result r1 = lin::verify_range(1, hi, o1);
    const lin::Result r4 = lin::verify_range(1, hi, o4);

    CHECK(r1.checksum == r4.checksum);
    CHECK(r1.max_excursion == r4.max_excursion);
    CHECK(r1.max_excursion_n0 == r4.max_excursion_n0);
    CHECK(r1.numbers_processed == r4.numbers_processed);
}

TEST_CASE("descent sieve matches the brute-force predicate for all residues, k<=12") {
    for (unsigned k = 1; k <= 12; ++k) {
        lin::Sieve2k sieve;
        sieve.build(k);
        const std::uint64_t size = std::uint64_t{1} << k;
        for (std::uint64_t r = 0; r < size; ++r) {
            REQUIRE(sieve.skip(static_cast<u128>(r)) == brute_skip(r, k));
        }
    }
}

TEST_CASE("descent sieve conventions") {
    lin::Sieve2k s2;
    s2.build(2);
    // All even residues are skipped.
    CHECK(s2.skip(static_cast<u128>(0)));
    CHECK(s2.skip(static_cast<u128>(2)));
    // Residue 1 mod 4 skipped; residue 3 mod 4 is a survivor at k=2.
    CHECK(s2.skip(static_cast<u128>(1)));
    CHECK_FALSE(s2.skip(static_cast<u128>(3)));

    // The residue is taken mod 2^k, so higher n with the same low bits agree.
    lin::Sieve2k s8;
    s8.build(8);
    for (u128 n = 3; n < 4096; n += 256) {
        CHECK(s8.skip(n) == s8.skip(n + 256));
    }

    // Disabled sieve never skips.
    lin::Sieve2k s0;
    s0.build(0);
    CHECK_FALSE(s0.skip(static_cast<u128>(4)));
}

TEST_CASE("mod-9 sieve skips exactly residues 2,4,5,8") {
    for (unsigned r = 0; r < 9; ++r) {
        const bool expected = (r == 2 || r == 4 || r == 5 || r == 8);
        CHECK(lin::mod9_skip(static_cast<u128>(r)) == expected);
        // And the same for larger representatives of the class.
        CHECK(lin::mod9_skip(static_cast<u128>(r) + 900) == expected);
    }
}
