// The excursion verify::run records for a walked n (collatz/verify/result.hpp,
// src/core/verify_records.cpp): the largest T^j(n) before the first value
// below n, computed here from the definition (excursion_dec). The block walk
// tests "< n" only at block ends; these blocks have walks that dip below n
// inside a block and are back above n at its end. Checked: every entry of the
// record list, the maximum and argmax with the list off, a list above a
// listed excursion, and the maximum over every member of the block.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "frontier_model/random_block.hpp"
#include "frontier_model/walk.hpp"
#include "scale.hpp"

#include <cstddef>
#include <doctest/doctest.h>
#include <random>
#include <string>

namespace v = collatz::verify;
using collatz::decimal_less;
using collatz::u128;
using collatz::frontier_test::excursion_dec;

namespace {

struct Max {
    std::string peak = "0";
    u128 n = 0;
};

// The largest excursion over [lo, hi), smallest n on ties.
Max brute_max(u128 lo, u128 hi) {
    Max m;
    for (u128 x = lo; x < hi; ++x) {
        const std::string q = excursion_dec(x);
        if (decimal_less(m.peak, q)) {
            m.peak = q;
            m.n = x;
        }
    }
    return m;
}

// Runs block `o` with the record list at "0" (every walked n), off, and above a
// listed excursion, against excursion_dec. Returns the list-off result.
v::Result check_block(v::Options o, std::mt19937_64& g) {
    o.record_threshold = "0";
    o.records_cap = std::size_t{1} << 20;
    const v::Result all = v::run(o);
    REQUIRE(all.records.size() == all.candidates_checked);
    Max walked;
    for (const v::RecordHit& h : all.records) {
        const std::string q = excursion_dec(h.n);
        CHECK_MESSAGE(h.peak == q, "n=" << collatz::to_string(h.n));
        if (decimal_less(walked.peak, q)) { // ascending n: the smallest n on ties
            walked.peak = q;
            walked.n = h.n;
        }
    }
    v::Options off = o;
    off.record_threshold.clear();
    off.threads = 1 + static_cast<unsigned>(g() % 3);
    const v::Result r = v::run(off);
    CHECK(v::max_excursion_decimal(r) == walked.peak);
    CHECK(r.max_excursion_n == walked.n);
    const bool wide = decimal_less(collatz::to_string(collatz::u128_max), walked.peak);
    CHECK(r.max_exceeds_u128 == wide);
    CHECK(r.max_excursion_wide == (wide ? walked.peak : std::string()));
    CHECK(r.coverage_checksum == all.coverage_checksum);
    CHECK(r.pow_checksum == all.pow_checksum);
    // The walked maximum is at most the maximum over every member, and equal
    // when the member that attains that one is walked.
    const u128 L = o.block_h << o.N;
    const Max every = brute_max(L, L + (static_cast<u128>(1) << o.N));
    CHECK_FALSE(decimal_less(every.peak, walked.peak));
    for (const v::RecordHit& h : all.records) {
        if (h.n == every.n) {
            CHECK(walked.peak == every.peak);
            CHECK(walked.n == every.n);
        }
    }
    if (!all.records.empty()) {
        v::Options above = o;
        above.record_threshold = all.records[g() % all.records.size()].peak;
        above.threads = 1 + static_cast<unsigned>(g() % 3);
        const v::Result s = v::run(above);
        std::size_t k = 0;
        for (const v::RecordHit& h : all.records) {
            if (decimal_less(above.record_threshold, h.peak)) {
                REQUIRE(k < s.records.size());
                CHECK(s.records[k].n == h.n);
                CHECK(s.records[k].peak == h.peak);
                ++k;
            }
        }
        CHECK(k == s.records.size());
    }
    return r;
}

v::Options cli_block(unsigned K, u128 H, unsigned A, unsigned B) {
    v::Options o;
    o.N = K;
    o.block_h = H;
    o.A = A;
    o.B = B;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.bv_cache = false;
    return o;
}

} // namespace

TEST_CASE("a walk back above n at a block end after a drop below n keeps its excursion") {
    std::mt19937_64 g(7);
    // n = 7684838176232295 drops below n at step 16 and is above n again at the
    // block ends 20, 36, 52 and 68 (M = 4); the block walk saw 3837437136265229792.
    const v::Result r = check_block(cli_block(6, 120075596503629ULL, 2, 10), g);
    CHECK(v::max_excursion_decimal(r) == "147715497873808874");
    CHECK(r.max_excursion_n == 7684838176232295ULL);
    // (H+1) * 2^6 * (3/2)^14 is above that maximum, so it is not provably exact.
    CHECK_FALSE(r.max_exact);
    v::Options o = cli_block(16, 966191429637633ULL, 8, 8);
    o.window_templates_D = 0;
    const v::Result r2 = check_block(o, g);
    CHECK(v::max_excursion_decimal(r2) == "1215544857389219597532884");
    CHECK(collatz::to_string(r2.max_excursion_n) == "63320321532731921823");
}

TEST_CASE("an excursion past 2^128 is the wide record and a record hit") {
    std::mt19937_64 g(11);
    u128 n_hp = 0; // hellpig's start, climbs to about 2^135.4
    REQUIRE(collatz::from_string("274133054632352106267", n_hp));
    v::Options o = cli_block(8, n_hp >> 8, 4, 8);
    o.window_templates_D = 0;
    const v::Result r = check_block(o, g);
    CHECK(r.max_exceeds_u128);
    CHECK(r.max_excursion_n == n_hp);
    CHECK(r.max_excursion_wide == "56649062372194325899121269007146717645316");
    CHECK(r.overflow_fallbacks > 0);
}

TEST_CASE("random small blocks record every walked excursion from the definition") {
    std::mt19937_64 g(0x5eedf1a7ULL);
    const unsigned cases = collatz::test::scaled(300, 30);
    for (unsigned c = 0; c < cases; ++c) {
        const v::Options o = collatz::frontier_test::random_block(g, {});
        CAPTURE(c);
        CAPTURE(collatz::to_string(o.block_h));
        CAPTURE(o.N);
        CAPTURE(o.A);
        CAPTURE(o.B);
        (void) check_block(o, g);
    }
}
