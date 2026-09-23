// verify::run end to end (src/core/verify.cpp): Step 2 against the A = 0
// reference, the parameter checks, thread independence, the maximum
// excursion against the path records and the linear verifier, and a sub-word B.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/linear.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "scale.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <stdexcept>
#include <vector>

namespace vf = collatz::verify;
using collatz::u128;
using collatz::test::scaled;

namespace {

// Path-record starting values with n < 2^24 (see tests/integration/test_linear_oracle.cpp).
// The maximum excursion over [1, 2^24) is attained at one of these.
constexpr std::uint64_t kPathRecords[] = {
    1,       2,       3,       7,       15,      27,      255,     447,     639,     703,
    1819,    4255,    4591,    9663,    20895,   26623,   31911,   60975,   77671,   113383,
    138367,  159487,  270271,  665215,  704511,  1042431, 1212415, 1441407, 1875711, 1988859,
    2643183, 2684647, 3041127, 3873535, 4637979, 5656191, 6416623, 6631675};

vf::Result run(unsigned N, unsigned threads) {
    vf::Options o;
    o.N = N;
    o.A = 0; // pure Step-1 (A==0) reference; the shipped default is A=10.
    o.threads = threads;
    return vf::run(o);
}

// Release builds use the full N; sanitizer builds shrink it so the debug run
// stays quick.
const unsigned kNsmall = scaled(20, 10);
const unsigned kNbig = scaled(24, 13);

} // namespace

// Step 2 (A > 0) must find the same global max excursion as the A == 0
// configuration (a high-excursion number climbs for many steps, so the Step 2
// look-ahead cannot prune it), while pruning enough extensions that it checks
// no more candidates than A == 0. Checksums are not expected to match across A
// (the candidate sets differ), but they must be thread-count independent for a
// fixed A.
TEST_CASE("verify Step 2 (A=6) matches A=0 max excursion and prunes candidates") {
    const vf::Result base = run(kNbig, 1); // A == 0 reference.

    vf::Options o6;
    o6.N = kNbig;
    o6.A = 6;
    o6.B = scaled(20, 14); // small look-ahead keeps the build cheap; must be >= 2.
    o6.num_bitvectors = 8;
    o6.threads = 1;
    const vf::Result r1 = vf::run(o6);

    CHECK(r1.max_excursion == base.max_excursion);
    CHECK(r1.max_excursion_n == base.max_excursion_n);
    CHECK(r1.candidates_checked <= base.candidates_checked);
    CHECK(r1.candidates_checked > 0);
    CHECK(r1.step2_masked_out > 0);
    CHECK(base.step2_masked_out == 0); // A == 0 prunes nothing via Step 2.

    // Thread-count independence for A = 6: every scalar identical at 1 vs 4.
    vf::Options o6t = o6;
    o6t.threads = 4;
    const vf::Result r4 = vf::run(o6t);
    CHECK(r4.candidates_checked == r1.candidates_checked);
    CHECK(r4.step2_masked_out == r1.step2_masked_out);
    CHECK(r4.coverage_checksum == r1.coverage_checksum);
    CHECK(r4.pow_checksum == r1.pow_checksum);
    CHECK(r4.step3_steps_total == r1.step3_steps_total);
    CHECK(r4.max_excursion == r1.max_excursion);
    CHECK(r4.max_excursion_n == r1.max_excursion_n);
    CHECK(r4.overflow_fallbacks == r1.overflow_fallbacks);
}

// Out-of-range Step 2 parameters are rejected (the Filter only asserts, which
// is compiled out under NDEBUG, so run() guards them explicitly).
TEST_CASE("run rejects out-of-range step2 parameters") {
    vf::Options o;
    o.N = 20;
    o.A = 15; // > step2::max_A (14)
    CHECK_THROWS_AS((void) vf::run(o), std::invalid_argument);
    o.A = 6;
    o.B = 1; // < 2
    CHECK_THROWS_AS((void) vf::run(o), std::invalid_argument);
    o.B = 24;
    o.A = 20; // A >= N
    CHECK_THROWS_AS((void) vf::run(o), std::invalid_argument);
}

// The candidate set, coverage and pow checksums, step total, max excursion and
// argmax, and overflow fallbacks are identical at 1 and 4 threads. table16
// reports 16-step block counts as the step total and fills the pow digest.
TEST_CASE("verify results are thread- and kernel-independent") {
    // Under sanitizers this checks the small N only; release also checks kNbig.
    const std::vector<unsigned> ns = collatz::test::sanitized()
                                         ? std::vector<unsigned>{kNsmall}
                                         : std::vector<unsigned>{kNsmall, kNbig};
    for (const unsigned N : ns) {
        CAPTURE(N);
        const vf::Result base = run(N, 1);
        const vf::Result r = run(N, 4);
        CHECK(r.candidates_checked == base.candidates_checked);
        CHECK(r.coverage_checksum == base.coverage_checksum);
        CHECK(r.max_excursion == base.max_excursion);
        CHECK(r.max_excursion_n == base.max_excursion_n);
        CHECK(r.overflow_fallbacks == base.overflow_fallbacks);
        // prefix_walks is an order-/thread-dependent perf diagnostic under
        // the threshold record, so it is intentionally not asserted equal.
        CHECK(r.step3_steps_total == base.step3_steps_total);
        CHECK(r.pow_checksum == base.pow_checksum);
        CHECK(base.blocks_total == base.step3_steps_total); // block-count sum mirror
        CHECK(base.pow_checksum != 0);                      // the block kernel populates pow
        CHECK(base.candidates_checked > 0);
        CHECK(base.overflow_fallbacks == 0);
    }
}

// The max excursion equals the GMP brute force over the path records and equals
// what the independent linear verifier reports. Runs at the reduced N under
// sanitizers.
TEST_CASE("verify max excursion matches path records and the linear verifier") {
    const vf::Result r = run(kNbig, 4);

    // (a) brute force over the hardcoded path-record list, via the oracle.
    const u128 hi = static_cast<u128>(1) << kNbig;
    u128 expect_max = 0;
    u128 expect_n = 0;
    for (const std::uint64_t rec : kPathRecords) {
        if (static_cast<u128>(rec) >= hi) {
            continue;
        }
        const collatz::step3::Outcome o = collatz::step3::finish_oracle(rec, rec);
        if (o.max_seen > expect_max || (o.max_seen == expect_max && rec < expect_n)) {
            expect_max = o.max_seen;
            expect_n = static_cast<u128>(rec);
        }
    }
    REQUIRE(expect_n != 0);
    CHECK(r.max_excursion == expect_max);
    CHECK(r.max_excursion_n == expect_n);

    // (b) the linear verifier over the same interval, with its sieves on, must
    // agree on the maximum and its argument.
    collatz::linear::Options lo;
    lo.sieve_bits = scaled(20, 14);
    lo.mod9 = true;
    lo.threads = 4;
    const collatz::linear::Result lr = collatz::linear::verify_range(1, hi, lo);
    CHECK(r.max_excursion == lr.max_excursion);
    CHECK(r.max_excursion_n == lr.max_excursion_n0);
}

// verify::run at a sub-word look-ahead (B < 6); tests/core/test_step2.cpp checks the
// mask itself.
TEST_CASE("verify runs at sub-word B (B < 6) with the same exact max as A == 0") {
    collatz::verify::Options base;
    base.N = 18;
    base.A = 0;
    base.threads = 1;
    // ctest-scoped cache only (never %LOCALAPPDATA%\collatz): see scoped_bv_cache().
    base.bv_cache = collatz::test::scoped_bv_cache();
    const collatz::verify::Result r0 = collatz::verify::run(base);
    for (unsigned B = 2; B <= 7; ++B) {
        collatz::verify::Options o;
        o.N = 18;
        o.A = 6;
        o.B = B;
        o.threads = 2;
        o.bv_cache = collatz::test::scoped_bv_cache();
        const collatz::verify::Result r = collatz::verify::run(o);
        INFO("B=" << B);
        CHECK(r.max_excursion == r0.max_excursion);
        CHECK(r.max_excursion_n == r0.max_excursion_n);
        CHECK(r.candidates_checked > 0u);
        CHECK(r.candidates_checked <= r0.candidates_checked);
    }
}
