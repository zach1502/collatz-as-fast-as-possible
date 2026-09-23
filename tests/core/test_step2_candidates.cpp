// The partial-OEE threshold a* (step1::Node::oee_astar and the prefix walk of
// tests/support/partial_oee_walk.hpp) at the D == 0 boundary.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "mpz_doctest.hpp"
#include "partial_oee_walk.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>

using collatz::u128;

// The partial-OEE a* certificate is strict: an extension n = n0 + a*2^M is
// excluded iff the merge target (m_j^{(a)} - 1)/2 is strictly less than n. At the
// boundary D = m_j - 1 - 2 n0 == 0 the target equals n0 itself (a self-merge, not
// a smaller number), so a = 0 must be kept and a*(j) must be >= 1. Mapping
// D == 0 to a* = 0 would clear the a = 0 candidate with no valid smaller merge
// target; the DFS and the walk test `d_lhs >= d_rhs` (D >= 0 => a* = D/C + 1 >= 1;
// D < 0 => a* = 0, the whole class merges below n0, sound).
//
// Reachability: a D == 0 arm needs T^j(n0) = 2 n0 + 1 on an OEE arm that completes
// (odd run then two evens) within depth M. An exhaustive search finds none for any
// n0 < 2^30 (nor any v = 2 n0 + 1 < 8*10^7 that both begins an odd^l-even-even
// pattern and lies in the forward orbit of (v-1)/2). So a wrong boundary stays
// latent, and the M <= 20 cross-impl test cannot catch it: no natural
// trajectory reaches an arm with D == 0 at testable scale. The tests below
// therefore (1) pin the certificate arithmetic at the boundary as a white-box
// mirror of the per-arm a*, (2) GMP-verify on the real near-boundary values
// (m_j = 2 n0 + 1 at n0 = 83, 125, 188) that the a = 0 target equals n0 exactly (so
// a = 0 is provably not excludable), and (3) cross-check the prefix walk against
// the strict-`<` GMP reference at those n0 for M = 27..30.

namespace {

// White-box mirror of the per-arm a* (step1_path.hpp leaf_oee_astar): the smallest
// excluded extension a is a* = (D >= 0) ? D/C + 1 : 0, with D = d_lhs - d_rhs and
// C > 0. Excluded set is the up-set {a : a*C > D}. The comparison is >=, not >.
u128 a_star_of(u128 d_lhs, u128 d_rhs, u128 c_pos) {
    if (d_lhs >= d_rhs) {
        return (d_lhs - d_rhs) / c_pos + 1;
    }
    return 0;
}

} // namespace

TEST_CASE("partial-OEE a* certificate: D == 0 keeps a = 0 (BLOCKER-1)") {
    // C is any positive slope; the boundary behavior is independent of it.
    for (const u128 c : {u128(1), u128(2), u128(7), u128(1024)}) {
        // D < 0 (target strictly below n0): the whole class merges, a = 0 pruned.
        CHECK(a_star_of(10, 11, c) == 0);
        // D == 0 (target == n0): a self-merge, a = 0 must be kept => a* == 1.
        CHECK(a_star_of(11, 11, c) == 1);
        CHECK(a_star_of(1000, 1000, c) == 1);
    }
    // D > 0: a* = floor(D/C) + 1, and a* - 1 must not be excluded while a* is.
    CHECK(a_star_of(20, 10, 7) == 10 / 7 + 1); // D=10, C=7 => 2
    CHECK(a_star_of(20, 10, 5) == 10 / 5 + 1); // D=10, C=5 => 3
}

TEST_CASE("real near-boundary values: (m_j - 1)/2 == n0 so a = 0 is not excludable") {
    // By exhaustive search, the only n0 < 2^30 with T^j(n0) = 2 n0 + 1 on an odd arm.
    struct Case {
        unsigned long n0;
        unsigned j;
    };
    const Case cases[] = {{83, 26}, {125, 26}, {188, 26}};
    for (const Case& c : cases) {
        // Walk to depth j with GMP; confirm m_j = 2 n0 + 1 and the OEE target
        // (m_j - 1)/2 equals n0 exactly -- not strictly smaller, so no valid
        // exclusion certificate exists for a = 0 (the representative).
        mpz_class m = c.n0;
        for (unsigned k = 0; k < c.j; ++k) {
            if (mpz_odd_p(m.get_mpz_t()) != 0) {
                m = (3 * m + 1) / 2;
            } else {
                m = m / 2;
            }
        }
        const mpz_class n0 = c.n0;
        const mpz_class two_n0_plus_1 = 2 * n0 + 1;
        CHECK(m == two_n0_plus_1);
        const mpz_class target = (m - 1) / 2;
        CHECK(target == n0);      // target == n, so a = 0 is a self-merge.
        CHECK_FALSE(target < n0); // strict certificate does not hold: keep a = 0.
    }
}

TEST_CASE("the prefix-walk a* agrees with the strict reference at near-boundary n0") {
    constexpr unsigned A = 6;
    // The near-boundary n0 occur at M >= 27 (j = 26); there the walk must agree
    // with the GMP reference.
    for (const unsigned M : {27u, 28u, 29u, 30u}) {
        collatz::ref::Options ro;
        ro.oee = true;
        ro.descent = false;
        ro.path_merge = false;
        for (const unsigned long n0v : {83UL, 125UL, 188UL}) {
            const u128 n0 = n0v;
            const std::uint32_t got = collatz::test::partial_oee_walk(n0, M, A);
            const unsigned want = collatz::ref::partial_oee_a_star(n0, M, A, ro);
            CAPTURE(M);
            CAPTURE(n0v);
            // Soundness contract: the walk excludes exactly the same extensions
            // as the strict-`<` GMP reference. (These n0 are pruned
            // by a D < 0 arm whose target is strictly below n0 -- sound -- not
            // by the D == 0 boundary arm, which does not complete here.)
            CHECK(static_cast<unsigned>(got) == want);
        }
    }
}
