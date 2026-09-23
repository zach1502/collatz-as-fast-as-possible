#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/linear.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <vector>

using collatz::u128;
namespace lin = collatz::linear;

namespace {

// Path-record starting values below 2^24, from the published Barina and
// Roosendaal path-record tables. They are the only n0 < 2^24 that can hold the
// maximum excursion of the interval.
constexpr std::uint64_t kPathRecords[] = {
    1,       2,       3,       7,       15,      27,      255,     447,     639,     703,
    1819,    4255,    4591,    9663,    20895,   26623,   31911,   60975,   77671,   113383,
    138367,  159487,  270271,  665215,  704511,  1042431, 1212415, 1441407, 1875711, 1988859,
    2643183, 2684647, 3041127, 3873535, 4637979, 5656191, 6416623, 6631675};

// Optimized builds exercise the full ranges; sanitizer builds use smaller ranges
// (still large enough to contain a path record) so the debug run finishes quickly.
const unsigned kCrossExp = collatz::test::scaled(26, 20);  // [1, 2^kCrossExp)
const unsigned kRecordExp = collatz::test::scaled(24, 21); // [1, 2^kRecordExp)

} // namespace

TEST_CASE("range verification is identical for 1 vs 8 threads") {
    const u128 hi = static_cast<u128>(1) << kCrossExp;
    lin::Options o1;
    o1.sieve_bits = 20;
    o1.mod9 = true;
    o1.threads = 1;
    lin::Options o8 = o1;
    o8.threads = 8;

    const lin::Result r1 = lin::verify_range(1, hi, o1);
    const lin::Result r8 = lin::verify_range(1, hi, o8);

    CHECK(r1.checksum == r8.checksum);
    CHECK(r1.max_excursion == r8.max_excursion);
    CHECK(r1.max_excursion_n0 == r8.max_excursion_n0);
    CHECK(r1.numbers_processed == r8.numbers_processed);
    CHECK(r1.numbers_skipped_by_sieve == r8.numbers_skipped_by_sieve);
    CHECK(r1.overflow_fallbacks == 0);
}

TEST_CASE("max excursion over the range is a known path record") {
    const u128 hi = static_cast<u128>(1) << kRecordExp;

    // Expected max/argmax computed with the GMP oracle over the hardcoded path
    // records that fall in range -- the same "until below n0" loop the verifier
    // runs, so the values are directly comparable.
    mpz_class expected_max = 0;
    u128 expected_n0 = 0;
    for (const std::uint64_t rec : kPathRecords) {
        if (static_cast<u128>(rec) >= hi) {
            continue;
        }
        mpz_class m;
        (void) lin::verify_one_oracle(static_cast<u128>(rec), m);
        if (m > expected_max) {
            expected_max = m;
            expected_n0 = static_cast<u128>(rec);
        }
    }
    REQUIRE(expected_n0 != 0);

    lin::Options opts;
    opts.sieve_bits = 0; // no sieve: the record itself must be processed
    opts.mod9 = false;
    opts.threads = 1;
    const lin::Result r = lin::verify_range(1, hi, opts);

    // The interval max equals the oracle max, and is attained at a path record.
    CHECK(collatz::oracle::to_mpz(r.max_excursion) == expected_max);
    CHECK(r.max_excursion_n0 == expected_n0);

    const bool is_record =
        std::find(std::begin(kPathRecords),
                  std::end(kPathRecords),
                  static_cast<std::uint64_t>(r.max_excursion_n0)) != std::end(kPathRecords);
    CHECK(is_record);
}

TEST_CASE("overflow falls back to the GMP oracle and stays correct") {
    // n0 = 2^127 - 1: n0+1 = 2^127 so ctz(n0+1) = 127, far beyond the POW3
    // table, forcing the very first 3^a multiply to overflow u128.
    const u128 n0 = (static_cast<u128>(1) << 127) - 1;

    const lin::OneResult r = lin::verify_one_detailed(n0);
    CHECK(r.overflowed);

    // The oracle is the source of truth for the checksum.
    mpz_class oracle_max;
    const std::uint64_t oracle_cs = lin::verify_one_oracle(n0, oracle_max);
    CHECK(r.checksum == oracle_cs);
    // The true excursion exceeds u128, so the fast result saturates.
    CHECK(mpz_sizeinbase(oracle_max.get_mpz_t(), 2) > 128);
    CHECK(r.max_seen == collatz::u128_max);

    // The range driver counts the fallback and reports the same checksum.
    lin::Options opts;
    opts.sieve_bits = 0;
    opts.mod9 = false;
    opts.threads = 1;
    const lin::Result rr = lin::verify_range(n0, n0 + 1, opts);
    CHECK(rr.overflow_fallbacks == 1);
    CHECK(rr.numbers_processed == 1);
    CHECK(rr.checksum == oracle_cs);
}
