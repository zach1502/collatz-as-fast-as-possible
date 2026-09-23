// Frontier block mode, property tier. Checks block runs of verify::run against
//   * the full run (H = 0 pins at K = 28/32/36),
//   * an independent block model (tests/support/frontier_model.hpp): ref/ Step 1
//     survivors (independent code, GMP), lifted by walking n0 + L directly, the
//     Step-2 decisions (mod 9, window dip from ref::dip_B, per-extension
//     partial OEE) evaluated on the concrete block member, and a
//     single-stepping Step-3 walk with a GMP fallback;
//   * the shipped configuration against that plain model: its candidate set is
//     a subset, and every shipped-only exclusion replays in GMP;
//   * a brute-force walk of every number in the block,
//   * the union of two half blocks.
// test_frontier_blocks_records.cpp checks the case-slice merge, a block
// holding a 128-bit overflow and the record list, and
// test_frontier_blocks_certify.cpp checks `certify` on a block.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "frontier_model.hpp"
#include "mpz_doctest.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <string>
#include <vector>

namespace v = collatz::verify;
using collatz::u128;
using namespace collatz::frontier_test;

TEST_CASE("frontier: H=0 is the full run (K=28/32/36 pins)") {
    struct Pin {
        unsigned K;
        std::uint64_t cand, masked, steps, cov, pow, blocks;
        const char* max;
        const char* argmax;
        bool exact;
        std::uint64_t survivors;
    };
    const Pin pins[] = {
        {28,
         595439,
         4596004,
         1784368,
         359801132691430922ULL,
         2034421027568454485ULL,
         1784368,
         "3202398580560632",
         "210964383",
         false,
         4942},
        {32,
         5559043,
         52403287,
         18506020,
         1723397994828173139ULL,
         1791159895143084074ULL,
         18506020,
         "3562942561397226080",
         "1410123943",
         true,
         56476},
        {36,
         71141351,
         602426250,
         256500920,
         1012906364919380615ULL,
         815100471492106042ULL,
         256500920,
         "102868194685920926084",
         "59436135663",
         true,
         657653},
    };
    for (const Pin& p : pins) {
        CAPTURE(p.K);
        v::Options o = shipped(p.K, 0, 24);
        o.record_threshold.clear();
        const v::Result r = v::run(o);
        CHECK(r.candidates_checked == p.cand);
        CHECK(r.step2_masked_out == p.masked);
        CHECK(r.step3_steps_total == p.steps);
        CHECK(r.coverage_checksum == p.cov);
        CHECK(r.pow_checksum == p.pow);
        CHECK(r.blocks_total == p.blocks);
        CHECK(collatz::to_string(r.max_excursion) == p.max);
        CHECK(collatz::to_string(r.max_excursion_n) == p.argmax);
        CHECK(r.max_exact == p.exact);
        CHECK_FALSE(r.max_exceeds_u128);
        CHECK(r.max_excursion_wide.empty());
        CHECK(r.overflow_fallbacks == 0);
        CHECK(r.step1.survivors == p.survivors);
        CHECK(r.pow_kernel_id == 16);
        CHECK(r.preflight_candidates == 131071);
        CHECK(r.preflight_coverage == 1869918586605918895ULL);
        CHECK(r.preflight_pow == 874001599976092917ULL);
        CHECK(r.preflight_blocks == 151749);
        CHECK(r.preflight_step3_steps == 151749);
        CHECK(r.records.empty());
    }
}

TEST_CASE("frontier: small blocks match the independent model and a brute-force walk") {
    const u128 Hs[] = {1, 3, 7, 12345, static_cast<u128>(3) << 40, pow2(50) + 1, pow2(60) + 12345};
    struct Case {
        unsigned K;
        u128 H;
    };
    std::vector<Case> cases;
    for (const u128 H : Hs) {
        cases.push_back({20, H});
    }
    // Larger K on a rotating subset (each block walk is 2^K numbers).
    cases.push_back({22, 3});
    cases.push_back({22, pow2(50) + 1});
    cases.push_back({24, 7});
    cases.push_back({24, static_cast<u128>(3) << 40});
    cases.push_back({26, 12345});
    cases.push_back({26, pow2(60) + 12345});
    std::size_t shipped_only = 0;
    for (const Case& c : cases) {
        CAPTURE(c.K);
        CAPTURE(collatz::to_string(c.H));
        const unsigned A = 10;
        const unsigned B = 16;
        const v::Result r = v::run(plain(c.K, c.H, B));
        const Model md = block_model(c.K, c.H, A, B, 8);
        check_against_model(r, md);
        // The shipped window terms only remove candidates, each soundly.
        shipped_only += check_shipped_vs_model(c.K, c.H, B, md);
        const u128 L = c.H << c.K;
        check_against_brute(r, md, L, L + pow2(c.K));
        // The block's exact-max guard uses the block end.
        CHECK(r.max_exact ==
              v::result_max_exact(c.K, c.H, v::unwalked_depth(plain(c.K, c.H, B)), r));
        // Thread count never changes a block result.
        const v::Result r1 = v::run(plain(c.K, c.H, B, 1));
        CHECK(r1.coverage_checksum == r.coverage_checksum);
        CHECK(r1.pow_checksum == r.pow_checksum);
        CHECK(r1.max_excursion_n == r.max_excursion_n);
    }
    CHECK(shipped_only > 0); // the replay above was exercised
}

TEST_CASE("frontier: blocks above a small L compose with the brute-force max below L") {
    // block_peak_compose: max over [1, L+W) of Q = max(G(L), R) once max(G, R)
    // exceeds the un-walked bound U = (L+W)(3/2)^K'. G(L) is brute-forced here.
    // B = 12 keeps K' = 22 small enough that the guard holds at L = 2^20.
    for (const u128 H : {static_cast<u128>(1), static_cast<u128>(3), static_cast<u128>(7)}) {
        CAPTURE(collatz::to_string(H));
        const unsigned K = 20;
        const v::Options o = shipped(K, H, 12);
        const v::Result r = v::run(o);
        // This shipped run against the plain model of the same block.
        (void) check_shipped_vs_model(K, H, 12, block_model(K, H, o.A, 12, o.num_bitvectors));
        const u128 L = H << K;
        const Brute below = brute_force(1, L);
        const Brute block = brute_force(L, L + pow2(K));
        const std::string R = v::max_excursion_decimal(r);
        const std::string composed = collatz::decimal_less(below.max_dec, R) ? R : below.max_dec;
        const std::string truth =
            collatz::decimal_less(below.max_dec, block.max_dec) ? block.max_dec : below.max_dec;
        // U = (L + W) * (3/2)^K' must be below max(G, R) for the statement.
        mpz_class U = collatz::oracle::to_mpz(L + pow2(K));
        const unsigned depth = v::unwalked_depth(o);
        mpz_class p3;
        mpz_ui_pow_ui(p3.get_mpz_t(), 3, depth);
        U *= p3;
        mpz_class lhs(composed, 10);
        mpz_mul_2exp(lhs.get_mpz_t(), lhs.get_mpz_t(), depth);
        REQUIRE(U < lhs);
        CHECK(composed == truth);
        MESSAGE("G(L)=" << below.max_dec << " R=" << R << " block brute max=" << block.max_dec);
    }
}

TEST_CASE("frontier: two half blocks at K equal their union at K+1 (max/argmax)") {
    // Each union holds a path record, which every configuration walks, so the
    // walked max is the true max of the union. Candidate sets and digests differ:
    // depth K+1 is a different Step-1 tree (M = K+1-A) with a deeper window, so
    // the sieves exclude different numbers.
    struct U {
        unsigned K;
        u128 h;
        const char* record; // path record start inside [h*2^(K+1), (h+1)*2^(K+1))
    };
    const U us[] = {
        {20, 0, "1988859"}, {20, 1, "3873535"}, {24, 1, "38595583"}, {24, 2, "80049391"}};
    for (const U& u : us) {
        CAPTURE(u.K);
        CAPTURE(collatz::to_string(u.h));
        const v::Result lo = v::run(shipped(u.K, 2 * u.h, 16));
        const v::Result hi = v::run(shipped(u.K, 2 * u.h + 1, 16));
        const v::Result un = v::run(shipped(u.K + 1, u.h, 16));
        const bool hi_wins =
            lo.max_excursion < hi.max_excursion ||
            (lo.max_excursion == hi.max_excursion && hi.max_excursion_n < lo.max_excursion_n);
        const v::Result& best = hi_wins ? hi : lo;
        CHECK(best.max_excursion == un.max_excursion);
        CHECK(best.max_excursion_n == un.max_excursion_n);
        CHECK(collatz::to_string(un.max_excursion_n) == u.record);
        MESSAGE("union K=" << u.K + 1 << " h=" << collatz::to_string(u.h) << ": candidates "
                           << lo.candidates_checked + hi.candidates_checked << " (two halves) vs "
                           << un.candidates_checked << " (union run)");
    }
}
