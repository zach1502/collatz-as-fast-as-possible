#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/certify.hpp"
#include "scale.hpp"

#include <doctest/doctest.h>

// Full certificate audit, property tier: recomputes the descent-or-merge
// certificate of every exclusion the verify pipeline makes at a small N and checks
// that each holds. test_exclusion_certificates.cpp checks a 1-in-64 sample instead.
//
// N=24 (M=14), smaller under sanitizers. The filter's O(2^B) build dominates the
// cost, so B is small.

TEST_CASE("full certificate audit is clean at small N (C2 property)") {
    collatz::certify::Options opts;
    opts.N = collatz::test::scaled(24, 20);
    opts.A = 10;
    opts.B = collatz::test::scaled(18, 12);
    opts.num_bitvectors = 8;
    opts.threads = collatz::test::scaled(4, 1);
    opts.mod3t = 6;
    opts.partial_oee = true;
    opts.replay_sample = 1; // full window replay at this scale.

    const collatz::certify::Report rep = collatz::certify::run(opts);

    for (const collatz::certify::Failure& f : rep.failures) {
        MESSAGE("FAIL n0=" << f.n0 << " n=" << f.n << " k=" << f.k << " " << f.reason);
    }
    CHECK(rep.failures_total == 0);
    CHECK(rep.clean());

    // The audit is not vacuous: Step 1 and Step 2 both exclude classes and
    // extensions at this N.
    CHECK(rep.s1_descent > 0);
    CHECK(rep.s1_path_merge > 0);
    CHECK(rep.s1_oee > 0);
    CHECK(rep.s2_survivors > 0);
    CHECK(rep.s2_excluded_total > 0);
    CHECK(rep.s2_candidates_checked > 0);
    // Full coverage was requested, so every window prune was replayed.
    CHECK(rep.s2_window_replayed == rep.s2_window_total);
}

TEST_CASE("mod-3^t Step-1 exclusions are certified when the sieve is on") {
    collatz::certify::Options opts;
    opts.N = collatz::test::scaled(24, 20);
    opts.A = 10;
    opts.B = collatz::test::scaled(18, 12);
    opts.num_bitvectors = 8;
    opts.threads = 1;
    opts.mod3t = 6;
    opts.partial_oee = true;

    const collatz::certify::Report rep = collatz::certify::run(opts);
    CHECK(rep.clean());
    // With t=6 enabled the generalized preimage-chain sieve fires; each hit is
    // independently re-certified by the backward-chain search.
    CHECK(rep.s1_mod3t > 0);
}
