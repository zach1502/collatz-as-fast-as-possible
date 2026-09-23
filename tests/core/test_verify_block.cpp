// Frontier block mode (collatz/verify/bounds.hpp, verify/records.hpp): the value
// width and block start, the block preconditions, the record threshold and
// assumption source, the block exact-max test and the record merge.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "block_mode.hpp"
#include "collatz/certify.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "mpz_doctest.hpp"
#include "step1_gmp.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>
#include <stdexcept>
#include <vector>

namespace v = collatz::verify;
using collatz::u128;
using collatz::test::block_opts;
using collatz::test::pow2;

namespace {

// The largest T-map value from n until the walk first drops below n: the
// excursion the verifier reports.
mpz_class t_peak_until_below(const mpz_class& n) {
    mpz_class x = n;
    mpz_class peak = n;
    do {
        x = collatz::test::step_T(x);
        peak = x > peak ? x : peak;
    } while (x >= n);
    return peak;
}

// The largest value of the 3n+1 map on the whole orbit of n down to 1.
mpz_class orbit_peak_3n1(const mpz_class& n) {
    mpz_class x = n;
    mpz_class peak = n;
    while (x != 1) {
        x = mpz_odd_p(x.get_mpz_t()) != 0 ? mpz_class(3 * x + 1) : mpz_class(x / 2);
        peak = x > peak ? x : peak;
    }
    return peak;
}

} // namespace

TEST_CASE("value_bits and block_start separate the depth from the value width") {
    v::Options o = block_opts(40, 0);
    CHECK(v::value_bits(o) == 40);
    CHECK(v::block_start(o) == 0);
    o.block_h = 1;
    CHECK(v::value_bits(o) == 41);
    CHECK(v::block_start(o) == pow2(40));
    o.block_h = static_cast<u128>(2075) << 20; // Barina's frontier 2075*2^60 at K=40
    CHECK(v::block_start(o) == static_cast<u128>(2075) << 60);
    CHECK(v::value_bits(o) == 72);
}

TEST_CASE("validate_block refuses what block mode does not support") {
    CHECK_NOTHROW(v::validate_block(block_opts(32, 0)));
    CHECK_NOTHROW(v::validate_block(block_opts(32, 1)));
    CHECK_NOTHROW(v::validate_block(block_opts(20, 1))); // L = 2^20 >= 2^17
    CHECK_THROWS_AS(v::validate_block(block_opts(16, 1)), std::invalid_argument); // L < 2^17
    v::Options o = block_opts(32, 1);
    o.A = 0;
    CHECK_THROWS_AS(v::validate_block(o), std::invalid_argument);
    o = block_opts(32, pow2(64)); // end (2^64+1)*2^32 > 2^96
    CHECK_THROWS_AS(v::validate_block(o), std::invalid_argument);
    o = block_opts(76, pow2(19), 6); // end 2^95 < 2^96, but (3/2)^70 * 2^95 >= 2^125
    CHECK_THROWS_AS(v::validate_block(o), std::invalid_argument);
    o = block_opts(32, 0);
    o.record_threshold = "012";
    CHECK_THROWS_AS(v::validate_block(o), std::invalid_argument);
    o.record_threshold = "12";
    CHECK_NOTHROW(v::validate_block(o));
    o.records_cap = 0;
    CHECK_THROWS_AS(v::validate_block(o), std::invalid_argument);
    // certify --block checks the same preconditions before building a filter.
    collatz::certify::Options co;
    co.N = 16;
    co.A = 6;
    co.B = 12;
    co.block_h = 1; // L = 2^16 < 2^17
    CHECK_THROWS_AS((void) collatz::certify::run(co), std::invalid_argument);
    co.N = 24;
    co.A = 0; // block mode needs A > 0
    CHECK_THROWS_AS((void) collatz::certify::run(co), std::invalid_argument);
}

TEST_CASE("Barina's record #97 peak, the default block threshold, recomputes in GMP") {
    u128 n = 0;
    REQUIRE(collatz::from_string(v::kBarinaRecord97N, n));
    CHECK(n < pow2(71)); // below every block that gets this default
    // T-map walk until the first drop below n: the excursion this verifier reports.
    const mpz_class peak = t_peak_until_below(collatz::oracle::to_mpz(n));
    CHECK(collatz::oracle::to_decimal(peak) == v::kBarinaRecord97PeakT);
    // Barina's list gives the 3n+1 maximum over the whole orbit: twice that.
    // mpz_class(...) turns the GMP expression template into the type that
    // mpz_doctest.hpp's StringMaker formats; doctest would otherwise print the
    // expression through libgmpxx's operator<<, which the tests do not link
    // (see collatz/oracle.hpp).
    CHECK(orbit_peak_3n1(collatz::oracle::to_mpz(n)) == mpz_class(2 * peak));
}

TEST_CASE("assumption source, assumes_below and the default threshold follow L vs 2^71") {
    v::Options o = block_opts(40, 0);
    CHECK(v::assumes_below(o) == "0");
    CHECK(v::assumption_source(o) == v::kAssumptionNone);
    CHECK(v::default_record_threshold(o).empty());
    o.block_h = static_cast<u128>(2075) << 20; // L = 2075 * 2^60, above 2^71
    CHECK(v::assumes_below(o) == collatz::to_string(static_cast<u128>(2075) << 60));
    CHECK(v::assumption_source(o) == v::kAssumptionPriorBlocks);
    CHECK(v::default_record_threshold(o) == v::kBarinaRecord97PeakT);
    o.block_h = pow2(31); // L = 2^71 exactly: Barina's bound, and the default list
    CHECK(v::assumption_source(o) == v::kAssumptionBarina71);
    CHECK(v::default_record_threshold(o) == v::kBarinaRecord97PeakT);
    o.block_h = pow2(31) - 1; // L < 2^71
    CHECK(v::assumption_source(o) == v::kAssumptionBarina71);
    CHECK(v::default_record_threshold(o).empty());
    o.assumption_source = "independent rerun of [0, 2^71)";
    CHECK(v::assumption_source(o) == "independent rerun of [0, 2^71)");
}

TEST_CASE("result_max_exact uses the block end (h+1)*2^N") {
    v::Result r;
    r.max_excursion = pow2(70);
    const unsigned depth = 40;
    // Full run: 2^40 * 3^40 < 2^40 * 2^70  <=>  3^40 < 2^70 (3^40 ~ 2^63.4).
    CHECK(v::result_max_exact(40, 0, depth, r));
    // Block h = 2^7: end 2^47, 2^47 * 3^40 = 2^110.4 > 2^110.
    CHECK_FALSE(v::result_max_exact(40, pow2(7), depth, r));
    // Wide record 2^140 restores exactness for the block.
    r.max_excursion = collatz::u128_max;
    r.max_exceeds_u128 = true;
    CHECK_FALSE(v::result_max_exact(40, pow2(7), depth, r)); // saturated, value unknown
    mpz_class w = 1;
    mpz_mul_2exp(w.get_mpz_t(), w.get_mpz_t(), 140);
    r.max_excursion_wide = collatz::oracle::to_decimal(w);
    CHECK(v::result_max_exact(40, pow2(7), depth, r));
    // No preflight shortcut in block mode.
    v::Result z;
    CHECK(v::result_max_exact(16, 0, 0, z));
    CHECK_FALSE(v::result_max_exact(16, 2, 0, z));
}

TEST_CASE("merge_records sorts by n and keeps the largest peak per n") {
    std::vector<v::RecordHit> hits = {{7, "100"}, {3, "50"}, {7, "1000"}, {5, "9"}, {7, "999"}};
    const std::vector<v::RecordHit> m = v::merge_records(hits);
    REQUIRE(m.size() == 3);
    CHECK(m[0].n == 3);
    CHECK(m[1].n == 5);
    CHECK(m[2].n == 7);
    CHECK(m[2].peak == "1000");
}
