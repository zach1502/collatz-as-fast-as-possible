#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>

// End-to-end pins of the Step-3 tail paths whose driver-level checks are in
// tests/core/test_step3_gate_fold.cpp, test_step3_persist.cpp and
// test_step3_u64lanes.cpp: every Result scalar of verify::run equals the pins.

using collatz::u128;
namespace vf = collatz::verify;

// Fold-inline: the u64 fast-lane spin folds each descended candidate's block
// count and pow digest term into DeferQueue::fw / fp, and flush_tail_batch
// skips its pass-2 fold for the batch (DeferQueue::folded). The result must
// equal the pass-2 fold bit for bit: every Result scalar at N=24 (1T/4T) and
// N=28 (release) equals the pins, the same values as test_integration_pins.
namespace fold_inline {

namespace {

vf::Result run_shipped(unsigned N, unsigned threads) {
    vf::Options o;
    o.N = N;
    o.A = 10;
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = threads;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    // ctest-scoped cache only (never %LOCALAPPDATA%\collatz): see scoped_bv_cache().
    o.bv_cache = collatz::test::scoped_bv_cache();
    return vf::run(o);
}

struct Pin {
    std::uint64_t candidates;
    std::uint64_t coverage;
    std::uint64_t blocks;
    std::uint64_t pow;
    const char* max;
    std::uint64_t argmax;
};

void check_pin(const vf::Result& r, const Pin& p) {
    u128 mx = 0;
    REQUIRE(collatz::from_string(p.max, mx));
    CHECK(r.candidates_checked == p.candidates);
    CHECK(r.coverage_checksum == p.coverage);
    CHECK(r.blocks_total == p.blocks);
    CHECK(r.step3_steps_total == p.blocks);
    CHECK(r.pow_checksum == p.pow);
    CHECK(r.max_excursion == mx);
    CHECK(r.max_excursion_n == static_cast<u128>(p.argmax));
    CHECK(r.overflow_fallbacks == 0);
}

} // namespace

TEST_CASE("fold-inline: end-to-end Result == pass-2 pins") {
    const Pin n24{168270,
                  1778626949996728569ULL,
                  272752ULL,
                  2063450954732183946ULL,
                  "30171305459816",
                  6631675};
    check_pin(run_shipped(24, 1), n24);
    check_pin(run_shipped(24, 4), n24);
#ifdef NDEBUG
    const Pin n28{598504,
                  2170764957591927706ULL,
                  1794038ULL,
                  2263447125582193448ULL,
                  "3202398580560632",
                  210964383};
    check_pin(run_shipped(28, 1), n28);
    check_pin(run_shipped(28, 4), n28);
#endif
}

} // namespace fold_inline

// Persistent u64 fast-lane spin (step3_persist.hpp, with its 16-B {3^f, c} block
// table). The spin layout must not change any outcome: every Result scalar at
// N=24 (1T/4T) and N=28 (release) equals the pins (the same values as the
// fold-inline pins above).
namespace spin {

namespace {

vf::Result run_shipped(unsigned N, unsigned threads) {
    vf::Options o;
    o.N = N;
    o.A = 10;
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = threads;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    // ctest-scoped cache only (never %LOCALAPPDATA%\collatz): see scoped_bv_cache().
    o.bv_cache = collatz::test::scoped_bv_cache();
    return vf::run(o);
}

struct Pin {
    std::uint64_t candidates;
    std::uint64_t coverage;
    std::uint64_t blocks;
    std::uint64_t pow;
    const char* max;
    std::uint64_t argmax;
};

void check_pin(const vf::Result& r, const Pin& p) {
    u128 mx = 0;
    REQUIRE(collatz::from_string(p.max, mx));
    CHECK(r.candidates_checked == p.candidates);
    CHECK(r.coverage_checksum == p.coverage);
    CHECK(r.blocks_total == p.blocks);
    CHECK(r.pow_checksum == p.pow);
    CHECK(r.max_excursion == mx);
    CHECK(r.max_excursion_n == static_cast<u128>(p.argmax));
    CHECK(r.overflow_fallbacks == 0);
}

} // namespace

TEST_CASE("persistent spin: end-to-end Result == pins") {
    const Pin n24{168270,
                  1778626949996728569ULL,
                  272752ULL,
                  2063450954732183946ULL,
                  "30171305459816",
                  6631675};
    check_pin(run_shipped(24, 1), n24);
    check_pin(run_shipped(24, 4), n24);
#ifdef NDEBUG
    const Pin n28{598504,
                  2170764957591927706ULL,
                  1794038ULL,
                  2263447125582193448ULL,
                  "3202398580560632",
                  210964383};
    check_pin(run_shipped(28, 1), n28);
    check_pin(run_shipped(28, 4), n28);
#endif
}

} // namespace spin

// u64 tail-batch columns with mixed u64 / u128 pushes: Step-1 case slice 7 of
// 2^40 at N=66 pushes candidates with n < 2^64 (u64 columns) and n >= 2^64
// (widened) into the same batches. Every Result scalar must equal the pins,
// which a tail batch with only u128 columns gives at the same options.

namespace {

std::string s(u128 v) {
    return collatz::to_string(v);
}

} // namespace

TEST_CASE("u64lanes: N=66 case slice 7 of 2^40 (mixed u64 / u128 pushes) == pins") {
    vf::Options o;
    o.N = 66;
    o.A = 9;
    o.B = 20;
    o.num_bitvectors = 8;
    o.threads = 1;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.bv_cache = false; // tests never touch the shared on-disk cache
    o.step1.case_c = 7;
    o.step1.case_k = std::uint64_t{1} << 40;
    const vf::Result r = vf::run(o);
    // Pins of this slice with the options above; its argmax is above 2^64.
    CHECK(r.candidates_checked == 3502072u);
    CHECK(r.coverage_checksum == 1398463803207976405ULL);
    CHECK(r.pow_checksum == 666027336124641454ULL);
    CHECK(r.blocks_total == 11443439u);
    CHECK(s(r.max_excursion) == "42632269238541236211940348538");
    CHECK(s(r.max_excursion_n) == "26004496731972042783");
    CHECK(r.overflow_fallbacks == 0);
}
