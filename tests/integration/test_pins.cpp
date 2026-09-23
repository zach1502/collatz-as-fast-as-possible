#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/linear.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdint>
#include <doctest/doctest.h>

// Pins of the verifier hot path: exact candidate counts, block counts, coverage
// checksums and max excursions for A=6, B=24, bitvectors=8 and for the
// pure-Step-1 path (A=0), with the table16 kernel. A change to the finisher,
// fold, prefix-max reconstruction or Step-2 mask that alters one bit of these
// values fails here.
//
// The pinned pow_checksum is the pow result digest; it changes with the digest
// definition. The other values (candidates, coverage, max excursion and argmax)
// do not depend on the digest. Values are single-thread;
// tests/core/test_verify_run.cpp checks thread independence.

using collatz::u128;
namespace vf = collatz::verify;

namespace {

vf::Result run(unsigned N, unsigned A) {
    vf::Options o;
    o.N = N;
    o.A = A;
    o.threads = 1;
    // The pins are for the configuration without high-bit partial OEE and
    // without the mod-3^t Step-1 sieve. The library Options default to it
    // (partial_oee=false, mod3t=0) but the CLI turns partial_oee on and sets
    // mod3t=6, so both are set explicitly here. A sieve changes which candidates
    // are folded, and so the checksums; a change of library default must not
    // move these pins.
    o.partial_oee = false;
    o.step1.mod3t = 0;
    return vf::run(o);
}

struct Expected {
    std::uint64_t candidates;
    std::uint64_t coverage;
    std::uint64_t block_digest; ///< block-count sum (table16 step3_steps_total / blocks_total).
    std::uint64_t pow_digest;   ///< pow result digest (table16 pow_checksum).
    const char* max_excursion;
    std::uint64_t max_excursion_n;
};

// Checks every pinned value: table16 reports the pow result digest in
// pow_checksum and the block-count sum in step3_steps_total / blocks_total.
void check(unsigned N, unsigned A, const Expected& e) {
    const vf::Result r = run(N, A);
    u128 mx = 0;
    REQUIRE(collatz::from_string(e.max_excursion, mx));

    CHECK(r.candidates_checked == e.candidates);
    CHECK(r.coverage_checksum == e.coverage);
    CHECK(r.max_excursion == mx);
    CHECK(r.max_excursion_n == static_cast<u128>(e.max_excursion_n));
    CHECK(r.overflow_fallbacks == 0);

    CHECK(r.step3_steps_total == e.block_digest);
    CHECK(r.blocks_total == e.block_digest);
    CHECK(r.pow_checksum == e.pow_digest);
    // prefix_walks is a perf diagnostic (record contenders); it is order- and
    // thread-dependent under the threshold record, so it is not pinned here.
}

} // namespace

TEST_CASE("verify N=24 checksums do not drift (CLI default A=6 and A=0)") {
    check(24,
          6,
          {166358,
           1304072516901899812ULL,
           274612ULL,
           2196334194486955433ULL,
           "30171305459816",
           6631675});
    // A=0 => M=24: a 2^24-class enumeration that is too slow under the debug
    // sanitizers (single-threaded, ASan). Its pins depend on the exact B, so the
    // sweep cannot be scaled down; like the N=28 fixtures below it runs only in
    // release builds.
#ifdef NDEBUG
    check(24,
          0,
          {341366,
           1101504536266887836ULL,
           532073ULL,
           1600474563174353389ULL,
           "30171305459816",
           6631675});
#endif
}

// N=28 is too slow under the debug sanitizers, so it runs in release builds only.
#ifdef NDEBUG
TEST_CASE("verify N=28 checksums do not drift (CLI default A=6 and A=0)") {
    check(28,
          6,
          {544564,
           1457670992776171515ULL,
           1543155ULL,
           299153587419499900ULL,
           "3202398580560632",
           210964383});
    check(28,
          0,
          {2560482,
           2150799590636697741ULL,
           5105206ULL,
           326173821049880849ULL,
           "3202398580560632",
           210964383});
}
#endif

// The library default is A=10, B=24. These fixtures pin the checksums of that
// default at N=24/28 (with the sieve configuration above: partial_oee=false,
// mod3t=0), so a one-bit change in the finisher, fold or mask under the default
// fails here too. run() sets A explicitly, so the A=6/A=0 fixtures above do not
// depend on the default.
TEST_CASE("verify N=24 checksums do not drift (shipped default A=10)") {
    check(24,
          10,
          {175826,
           615434314073628993ULL,
           296375ULL,
           1713140087117576329ULL,
           "30171305459816",
           6631675});
}

#ifdef NDEBUG
TEST_CASE("verify N=28 checksums do not drift (shipped default A=10)") {
    check(28,
          10,
          {702389,
           1155321211291313032ULL,
           2144775ULL,
           1219523119989083603ULL,
           "3202398580560632",
           210964383});

    // The pinned N=28 max excursion against the independent linear verifier over
    // the whole interval [1, 2^28), as in test_integration_verify_oracle: the
    // pinned max is the true maximum, not only a stable digest.
    const u128 hi = static_cast<u128>(1) << 28;
    collatz::linear::Options lo;
    lo.sieve_bits = 20;
    lo.mod9 = true;
    lo.threads = 8;
    const collatz::linear::Result lr = collatz::linear::verify_range(1, hi, lo);

    const vf::Result r = run(28, 10);
    CHECK(r.max_excursion == lr.max_excursion);
    CHECK(r.max_excursion_n == lr.max_excursion_n0);
}
#endif

// Pins of the CLI configuration end to end. run_shipped() uses the CLI's A=10,
// B=24, bitvectors=8, kernel=table16, partial_oee on, mod3t=6 and
// window_chains=6; the fixtures above turn the partial-OEE and mod-3^t sieves
// off. These pins use the table16 kernel and a partial-OEE a* that keeps a=0 at
// D==0. A change to that sieve moves only the partial_oee=true pins; the
// partial_oee=false pins above must stay bit-identical, which the invariance
// check at the end asserts.

vf::Result run_shipped(unsigned N) {
    vf::Options o;
    o.N = N;
    o.A = 10;
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = 1;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    return vf::run(o);
}

// CLI configuration: table16 reports the block-count sum in step3_steps_total
// and blocks_total, and the pow result digest in pow_checksum.
void check_shipped(unsigned N, const Expected& e) {
    const vf::Result r = run_shipped(N);
    u128 mx = 0;
    REQUIRE(collatz::from_string(e.max_excursion, mx));
    CHECK(r.candidates_checked == e.candidates);
    CHECK(r.coverage_checksum == e.coverage);
    CHECK(r.max_excursion == mx);
    CHECK(r.max_excursion_n == static_cast<u128>(e.max_excursion_n));
    CHECK(r.overflow_fallbacks == 0);
    CHECK(r.step3_steps_total == e.block_digest);
    CHECK(r.blocks_total == e.block_digest);
    CHECK(r.pow_checksum == e.pow_digest);
}

TEST_CASE("verify N=24 SHIPPED config pinned (A=10, partial_oee=on, mod3t=6, window_chains=6, "
          "table16)") {
    // window_chains=6 shrinks the candidate set and moves the coverage/pow/block
    // digests; the exact global max stays the same (chains only prune
    // early-merging n).
    check_shipped(24,
                  {168270,
                   1778626949996728569ULL,
                   272752ULL,
                   2063450954732183946ULL,
                   "30171305459816",
                   6631675});

    // Invariance: with the partial-OEE and mod-3^t sieves off, the run must give
    // the pinned partial_oee=false A=10 coverage exactly (candidates and
    // checksums): the CLI sieves only remove candidates and never change the
    // coverage of the ones the sieve-off configuration checks.
    const vf::Result off = run(24, 10); // partial_oee=false, mod3t=0.
    CHECK(off.candidates_checked == 175826);
    CHECK(off.coverage_checksum == 615434314073628993ULL);
    // The CLI configuration checks strictly fewer candidates (sieves only prune).
    CHECK(run_shipped(24).candidates_checked < off.candidates_checked);
}

#ifdef NDEBUG
TEST_CASE("verify N=28 SHIPPED config pinned (A=10, partial_oee=on, mod3t=6, window_chains=6, "
          "table16)") {
    check_shipped(28,
                  {598504,
                   2170764957591927706ULL,
                   1794038ULL,
                   2263447125582193448ULL,
                   "3202398580560632",
                   210964383});

    // End-to-end oracle cross-check of the CLI configuration: the max excursion
    // over the whole interval must equal the independent linear verifier's.
    const u128 hi = static_cast<u128>(1) << 28;
    collatz::linear::Options lo;
    lo.sieve_bits = 20;
    lo.mod9 = true;
    lo.threads = 8;
    const collatz::linear::Result lr = collatz::linear::verify_range(1, hi, lo);
    const vf::Result r = run_shipped(28);
    CHECK(r.max_excursion == lr.max_excursion);
    CHECK(r.max_excursion_n == lr.max_excursion_n0);

    // Invariance at N=28: sieve-off reproduces the pinned partial_oee=false
    // coverage exactly, and the CLI configuration prunes strictly more.
    const vf::Result off = run(28, 10);
    CHECK(off.candidates_checked == 702389);
    CHECK(off.coverage_checksum == 1155321211291313032ULL);
    CHECK(r.candidates_checked < off.candidates_checked);
}
#endif
