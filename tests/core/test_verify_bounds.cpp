// collatz/verify/bounds.hpp (src/core/verify_bounds.cpp): the exact-max bound,
// the block preconditions of validate_block, and the command-line limits.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/certify.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <stdexcept>
#include <string>

// Exact-max soundness. The verifier reports an exact global max excursion, but
// numbers excluded by a Step-1 class certificate (steps < M) or a Step-2 window
// certificate (steps M..M+B) are never walked, so their peaks are unobserved.
// result_max_exact() is true iff the record strictly exceeds the un-walked
// peak bound 2^N*(3/2)^K, K = unwalked_depth(opts); then no unwalked number can
// reach it and the record is the exact global max. These tests pin that
// arithmetic against the published Barina/Roosendaal path records at the
// shipped depths, plus the strictness, the edge cases and the depth formula.
namespace {

collatz::u128 dec(const std::string& s) {
    collatz::u128 v = 0;
    REQUIRE(collatz::from_string(s, v));
    return v;
}

// Look-ahead schedule for the depths below: B = 31 (N>=44), 28 (40..43, the
// --gpu value; the CPU auto-B there is 30), 24 (below).
// M = N - A with A = 10; depth K = M + B.
unsigned shipped_depth(unsigned N) {
    const unsigned A = 10, M = N - A;
    const unsigned B = (N >= 44) ? 31u : (N >= 40) ? 28u : 24u;
    return M + B;
}

// result_max_exact() for a full run (block_h = 0) whose record is R.
bool exact_full(unsigned N, unsigned depth, collatz::u128 R, bool exceeds_u128 = false) {
    collatz::verify::Result r;
    r.max_excursion = R;
    r.max_exceeds_u128 = exceeds_u128;
    return collatz::verify::result_max_exact(N, 0, depth, r);
}

} // namespace

TEST_CASE("result_max_exact matches the published records at shipped depths") {
    // Largest path record with start < 2^N, and its peak t(n). true iff the
    // record beats the un-walked peak bound 2^N*(3/2)^{M+B}.
    struct Row {
        unsigned N;
        const char* record; // t(n) of the largest-start record below 2^N
        bool expect_exact;
    };
    const Row rows[] = {
        // Below the crossover: record does not beat the bound, so the flag is
        // false although the maximum is the published record (an under-claim,
        // not a defect).
        {20, "45119577824", false},      // record #25, n=1042431
        {24, "30171305459816", false},   // record #37, n=6631675
        {28, "3202398580560632", false}, // record #42, n=210964383
        // At and above the crossover: record beats the bound -> provably exact.
        {32, "3562942561397226080", true},         // record #44, n=1410123943
        {36, "102868194685920926084", true},       // record #51, n=59436135663
        {40, "200279370410625061016864", true},    // record #60, n=871673828443
        {44, "126114763591721667597212096", true}, // record #63, n=9016346070511
    };
    for (const Row& r : rows) {
        const unsigned K = shipped_depth(r.N);
        const collatz::u128 R = dec(r.record);
        CAPTURE(r.N);
        CAPTURE(K);
        CHECK(exact_full(r.N, K, R) == r.expect_exact);
    }
}

TEST_CASE("result_max_exact is strict at the bound (ties are not exact)") {
    // N=18 (above the preflight's 2^17, which makes every smaller N exact),
    // K=4: the bound test is 2^18 * 3^4 < 2^4 * R, i.e. R > 2^14 * 81 = 1327104.
    // So R=1327105 is exact, R=1327104 (equality) is not (an un-walked value
    // could equal the record at a smaller n), R=1327103 is not.
    CHECK(exact_full(18, 4, 1327105) == true);
    CHECK(exact_full(18, 4, 1327104) == false);
    CHECK(exact_full(18, 4, 1327103) == false);
    // depth 0: bound = 2^N, so R must strictly exceed 2^N.
    CHECK(exact_full(18, 0, 262145) == true);
    CHECK(exact_full(18, 0, 262144) == false);
}

TEST_CASE("result_max_exact edge cases") {
    // A u128-overflowing excursion without its exact value is never certified
    // exact by this bound.
    CHECK(exact_full(60, 90, collatz::u128_max, /*exceeds_u128=*/true) == false);
    // No excursion observed (empty range).
    CHECK(exact_full(40, 58, 0) == false);
}

TEST_CASE("unwalked_depth follows M + B") {
    using collatz::verify::unwalked_depth;
    collatz::verify::Options o;
    o.N = 40;
    o.A = 10;
    o.B = 28;
    CHECK(unwalked_depth(o) == 30u + 28u); // M + B
    o.A = 0; // no Step-2 window; only the M-step class prefix (M == N)
    CHECK(unwalked_depth(o) == 40u);
}

TEST_CASE("unwalked_bound_log2 tracks N + depth*(log2 3 - 1)") {
    using collatz::verify::unwalked_bound_log2;
    // A full run (block_h = 0). N=40 at depth 58 (B=28) -> ~2^73.9.
    CHECK(unwalked_bound_log2(40, 58, 0) == doctest::Approx(73.928).epsilon(0.001));
    CHECK(unwalked_bound_log2(32, 46, 0) == doctest::Approx(58.908).epsilon(0.001));
    // depth 0 -> exactly N.
    CHECK(unwalked_bound_log2(50, 0, 0) == doctest::Approx(50.0));
}

TEST_CASE("run() sets max_exact: preflight walks every n at small N") {
    // N <= kPreflightBits (17): the preflight walks every n < 2^N directly, so
    // the max is exact unconditionally (kept tiny so the ASan/UBSan run is fast).
    collatz::verify::Options o;
    o.N = 12;
    o.A = 0; // no Step-2 filter build (fast under ASan); each survivor is its own candidate
    o.threads = 1;
    const collatz::verify::Result r16 = collatz::verify::run(o);
    CHECK(r16.max_exact == true);
    // The standalone predicate agrees with what run() recorded.
    CHECK(r16.max_exact ==
          collatz::verify::result_max_exact(o.N, 0, collatz::verify::unwalked_depth(o), r16));
}

TEST_CASE("validate_block refuses a block whose M = K - A is below 2") {
    // Step 1 starts at depth 2, so at M = 1 it enumerates no class. The fuzzed
    // block H=32768 K=2 A=1 B=2, [131072, 131076), came back verified with
    // candidates_checked 0: 131075 = 3 (mod 4) had neither a walk nor a
    // certificate, and certify reported the block clean.
    collatz::verify::Options o;
    o.N = 2;
    o.block_h = 32768;
    o.A = 1;
    o.B = 2;
    o.bv_cache = false;
    CHECK_THROWS_AS(collatz::verify::validate_block(o), std::invalid_argument);
    CHECK_THROWS_AS((void) collatz::verify::run(o), std::invalid_argument);
    collatz::certify::Options co;
    co.N = 2;
    co.A = 1;
    co.B = 2;
    co.block_h = 32768;
    co.window_chains = 6;
    co.replay_sample = 1;
    CHECK_THROWS_AS((void) collatz::certify::run(co), std::invalid_argument);
    // M = 1 at a larger K, and an A near 2^32, which N - A must not wrap.
    o.N = 33;
    o.A = 32;
    o.block_h = 1;
    CHECK_THROWS_AS(collatz::verify::validate_block(o), std::invalid_argument);
    o.A = 0xFFFFFFFFu;
    CHECK_THROWS_AS(collatz::verify::validate_block(o), std::invalid_argument);
    // M = 2, the smallest block Step 1 enumerates: its one class (3 mod 4)
    // survives, and each of the survivor's 2^A extensions is walked or masked.
    o.N = 5;
    o.A = 3;
    o.block_h = 32768;
    CHECK_NOTHROW(collatz::verify::validate_block(o));
    const collatz::verify::Result r = collatz::verify::run(o);
    CHECK(r.step1.survivors == 1);
    CHECK(r.candidates_checked + r.step2_masked_out == 8);
    CHECK(r.candidates_checked > 0);
}

TEST_CASE("the --threads and --bitvectors limits") {
    // 4 threads per hardware thread, at least 256; 0 (all) is always accepted.
    const unsigned hw = collatz::hardware_threads();
    CHECK(collatz::verify::max_threads() >= 256u);
    CHECK(collatz::verify::max_threads() >= 4u * hw);
    CHECK(collatz::verify::max_threads() == std::max(256u, 4u * hw));
    // The lease and the command line share the bitvector bound.
    CHECK(collatz::verify::kMaxBitvectors == 32u);
}
