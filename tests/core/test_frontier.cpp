// The survivor lift of frontier block mode (collatz/frontier.hpp).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "block_mode.hpp"
#include "collatz/collatz_fn.hpp"
#include "collatz/frontier.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "partial_oee_walk.hpp"
#include "step1_gmp.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <vector>

using collatz::u128;
using collatz::test::pow2;

TEST_CASE("the survivor lift equals the walked block member") {
    // For every survivor at depth M and a block h, the lifted node must be the
    // class of n0 + L: m' = T^M(n0 + L) by direct walk, the same parity count f,
    // pmax' bounding the walked prefix of every extension, and a*' equal to
    // the OEE prefix walk on the lifted representative.
    const unsigned K = 22;
    const unsigned A = 8;
    const unsigned M = K - A;
    collatz::step1::Options s1;
    s1.mod3t = 6;
    s1.track_oee_astar = true;
    const std::vector<collatz::step1::Node> surv = collatz::test::survivors(M, s1);
    REQUIRE(surv.size() > 100);
    for (const u128 h : {static_cast<u128>(1),
                         static_cast<u128>(5),
                         static_cast<u128>(12345),
                         (static_cast<u128>(1) << 50) + 1}) {
        const collatz::frontier::Lift lift = collatz::frontier::make_lift(h, K, A);
        CHECK(lift.L == h * pow2(K));
        CHECK(lift.h2A == h * pow2(A));
        std::size_t prunes_checked = 0;
        for (std::size_t i = 0; i < surv.size(); i += 7) {
            collatz::step1::Node node = surv[i];
            collatz::frontier::lift_survivor(node, lift);
            CHECK(node.n0 == surv[i].n0 + lift.L);
            u128 vv = node.n0;
            unsigned f = 0;
            for (unsigned j = 0; j < M; ++j) {
                f += static_cast<unsigned>(vv & 1u);
                vv = collatz::T(vv).value;
            }
            CHECK(vv == node.m);
            CHECK(f == node.f);
            // The prefix bound covers the extremes of the 2^A extensions.
            for (const u128 a : {static_cast<u128>(0), pow2(A) - 1}) {
                u128 w = node.n0 + (a << M);
                u128 mx = w;
                for (unsigned j = 0; j < M; ++j) {
                    w = collatz::T(w).value;
                    mx = w > mx ? w : mx;
                }
                CHECK(mx <= node.pmax + node.smax * a);
            }
            const std::uint32_t walked = collatz::test::partial_oee_walk(node.n0, M, A);
            const u128 twoA = pow2(A);
            const u128 lifted = node.oee_astar >= twoA ? twoA : node.oee_astar;
            const u128 expect = walked == std::numeric_limits<std::uint32_t>::max() ? twoA : walked;
            CHECK(lifted == expect);
            prunes_checked += lifted < twoA ? 1u : 0u;
        }
        MESSAGE("h=" << collatz::to_string(h) << ": a* prunes checked " << prunes_checked);
    }
}

TEST_CASE("the lift moves a* in (h*2^A, (h+1)*2^A) to 0 < a*' < 2^A, and the mask follows") {
    // Survivors at depths M <= 26 carry a* = 0 or no threshold (each completing
    // arm's target is already below the leaf representative), so the threshold
    // is set by hand on a real survivor. A
    // raw x = h*2^A + r excludes the original extensions a >= x, which in block
    // h are the lifted extensions a' >= r: the lift must give a*' = r and
    // Filter::mask must then keep exactly the a' < r its unpruned mask keeps.
    // The neighbouring blocks get a*' = 2^A + r (nothing pruned) and 0 (all).
    const unsigned K = 22;
    const unsigned A = 8;
    const unsigned M = K - A;
    const u128 twoA = pow2(A);
    collatz::step1::Options s1;
    s1.mod3t = 6;
    s1.track_oee_astar = true;
    const std::vector<collatz::step1::Node> surv = collatz::test::survivors(M, s1);
    collatz::step2::Params p{K, A, 16, 8, 1u, true};
    const collatz::step2::Filter filter(p);
    const auto lifted = [&](collatz::step1::Node node, u128 h, u128 astar) {
        node.oee_astar = astar;
        collatz::frontier::lift_survivor(node, collatz::frontier::make_lift(h, K, A));
        return node;
    };
    std::size_t tested = 0;
    for (const u128 h : {static_cast<u128>(1), static_cast<u128>(5), pow2(40) + 3}) {
        for (const u128 r : {static_cast<u128>(1), static_cast<u128>(77), twoA - 1}) {
            CAPTURE(collatz::to_string(h));
            CAPTURE(collatz::to_string(r));
            const u128 x = h * twoA + r;
            // A survivor whose unpruned block mask keeps extensions on both
            // sides of r, so the comparison below sees both outcomes.
            for (std::size_t i = 0; i < surv.size(); i += 3) {
                const collatz::step1::Node none = lifted(surv[i], h, collatz::step1::kOeeAStarNone);
                const collatz::step2::CandidateMask ref = filter.mask(none);
                bool below = false;
                bool above = false;
                for (unsigned a = 0; a < twoA; ++a) {
                    below = below || (ref.test(a) && a < r);
                    above = above || (ref.test(a) && a >= r);
                }
                if (!below || !above) {
                    continue;
                }
                const collatz::step1::Node in = lifted(surv[i], h, x);
                CHECK(in.oee_astar == r);
                const collatz::step2::CandidateMask m = filter.mask(in);
                for (unsigned a = 0; a < twoA; ++a) {
                    CHECK(m.test(a) == (ref.test(a) && a < r));
                }
                const collatz::step1::Node prev = lifted(surv[i], h - 1, x);
                CHECK(prev.oee_astar == twoA + r);
                const collatz::step1::Node next = lifted(surv[i], h + 1, x);
                CHECK(next.oee_astar == 0);
                const collatz::step2::CandidateMask mn = filter.mask(next);
                for (unsigned a = 0; a < twoA; ++a) {
                    CHECK_FALSE(mn.test(a));
                }
                ++tested;
                break;
            }
        }
    }
    CHECK(tested == 9);
}
