// Frontier block mode, property tier: `certify` on a block (every exclusion
// re-derived on the block's numbers) and two simulated pipeline bugs it must
// report.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/certify.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "frontier_model.hpp"
#include "test_env.hpp"

#include <doctest/doctest.h>

namespace v = collatz::verify;
using collatz::u128;
using namespace collatz::frontier_test;

namespace {

/// `certify` with verify's defaults (A=10, auto-B 24 below K=40, mod3t 6,
/// partial OEE, window_chains 6, D = 12) on block (K, H), full replay.
collatz::certify::Options certify_block_opts(unsigned K, u128 H) {
    collatz::certify::Options co;
    co.N = K;
    co.block_h = H;
    co.A = 10;
    co.B = v::auto_step2_B(K, false);
    co.num_bitvectors = 8;
    co.threads = 4;
    co.mod3t = 6;
    co.partial_oee = true;
    co.window_chains = 6;
    co.window_templates_D = 12;
    co.replay_sample = 1;
    co.bv_cache = collatz::test::scoped_bv_cache();
    return co;
}

} // namespace

TEST_CASE("frontier: certify audits a block on its own numbers (K=26, H=2^60+12345)") {
    const unsigned K = 26;
    const u128 H = pow2(60) + 12345;
    const collatz::certify::Options co = certify_block_opts(K, H);
    const collatz::certify::Report rep = collatz::certify::run(co);
    for (const collatz::certify::Failure& f : rep.failures) {
        MESSAGE("FAIL n0=" << f.n0 << " n=" << f.n << " k=" << f.k << " " << f.reason);
    }
    CHECK(rep.failures_total == 0);
    CHECK(rep.clean());
    CHECK(rep.L == (H << K));
    // Every Step-1 class exclusion was also re-derived at its two block ends.
    CHECK(rep.s1_block_checked == rep.s1_descent + rep.s1_path_merge + rep.s1_oee + rep.s1_mod3t);
    CHECK(rep.s1_descent > 0);
    CHECK(rep.s1_path_merge > 0);
    CHECK(rep.s1_oee > 0);
    CHECK(rep.s1_mod3t > 0);
    CHECK(rep.s2_mod9 > 0);
    CHECK(rep.s2_window_replayed == rep.s2_window_total);
    CHECK(rep.s2_window_chain > 0); // the window chain/template terms fired and replayed
    // The audit judged exactly the decisions the block run made.
    const v::Result r = v::run(shipped(K, H, co.B));
    CHECK(rep.s2_survivors == r.step1.survivors);
    CHECK(rep.s2_candidates_checked == r.candidates_checked);
    CHECK(rep.s2_excluded_total == r.step2_masked_out);
    MESSAGE("block certify: " << rep.s2_excluded_total << " exclusions (" << rep.s2_window_total
                              << " window replays, " << rep.s2_window_chain
                              << " by a backward chain), " << rep.s1_block_checked
                              << " Step-1 classes at both block ends");
}

TEST_CASE("frontier: certify catches a dropped candidate and an unlifted mask") {
    const unsigned K = 26;
    const u128 H = pow2(60) + 12345;
    collatz::certify::Options co = certify_block_opts(K, H);
    // The block's record holder is a candidate the run walked; dropping it as
    // if Step 2 had excluded it must fail the audit, and only there.
    const v::Result r = v::run(shipped(K, H, co.B));
    co.test_drop_n = r.max_excursion_n;
    const collatz::certify::Report rep = collatz::certify::run(co);
    CHECK(rep.failures_total == 1);
    REQUIRE(rep.failures.size() == 1);
    CHECK(rep.failures[0].n == collatz::to_string(r.max_excursion_n));
    CHECK(rep.s2_candidates_checked + 1 == r.candidates_checked);
    // Step-2 decisions from the unlifted survivors (the full-run residue's mod-9
    // row, window start and a*), checked against the block's numbers: the
    // hazard the survivor lift removes. Every failing replay walks its whole
    // backward-chain search, so a deterministic 1-in-64 sample is replayed.
    co.test_drop_n = 0;
    co.test_unlifted_mask = true;
    co.replay_sample = 64;
    const collatz::certify::Report rep2 = collatz::certify::run(co);
    CHECK(rep2.failures_total > 0);
    MESSAGE("unlifted masks: " << rep2.failures_total
                               << " sampled exclusions without a certificate");
}
