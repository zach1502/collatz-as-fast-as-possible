#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <vector>

// Cross-implementation equality for high-bit partial OEE: the threshold a* that
// the Step-1 DFS carries (step1::Node::oee_astar, with
// step1::Options::track_oee_astar) must agree, survivor for survivor, with the
// definitional GMP reference collatz::ref::partial_oee_a_star, which forms every
// candidate n = n0 + a*2^M and tests the OEE certificate literally.
//
// The reference is written from the mathematics, independently of the optimized
// source. Agreement over all Step-1 survivors at several M pins down the affine
// a*(j) formula, its off-by-one, and the "arm completes by M" cutoff.

using collatz::u128;
using collatz::step1::Node;

namespace {

constexpr unsigned kNone = std::numeric_limits<std::uint32_t>::max();

// Every Step-1 survivor mod 2^M with its carried a*, sorted by n0.
std::vector<Node> survivors_with_a_star(unsigned M) {
    collatz::step1::Options so;
    so.track_oee_astar = true;
    std::vector<Node> out;
    collatz::step1::enumerate(M, so, [&out](const Node& n) { out.push_back(n); });
    std::sort(out.begin(), out.end(), [](const Node& a, const Node& b) { return a.n0 < b.n0; });
    return out;
}

// The carried a* as Filter::mask applies it at A: below 2^A it excludes every
// a >= a*; any larger value excludes nothing, which the reference reports as
// kNone.
unsigned carried_a_star(const Node& s, unsigned A) {
    if (s.oee_astar < (static_cast<u128>(1) << A)) {
        return static_cast<unsigned>(s.oee_astar);
    }
    return kNone;
}

// The reference with only the OEE certificate enabled.
collatz::ref::Options oee_only() {
    collatz::ref::Options ro;
    ro.descent = false;
    ro.path_merge = false;
    return ro;
}

// Compares the carried a* with the reference on every Step-1 survivor mod 2^M.
void compare_all(unsigned M, unsigned A) {
    const collatz::ref::Options ro = oee_only();
    std::uint64_t nontrivial = 0;
    for (const Node& s : survivors_with_a_star(M)) {
        const unsigned got = carried_a_star(s, A);
        const unsigned want = collatz::ref::partial_oee_a_star(s.n0, M, A, ro);
        if (got != want) {
            CAPTURE(M);
            CAPTURE(A);
            CAPTURE(collatz::to_string(s.n0));
            CAPTURE(got);
            CAPTURE(want);
            CHECK(got == want);
        }
        if (got != kNone) {
            ++nontrivial;
        }
    }
    // Some survivor is excluded at the larger M, so the comparison is not vacuous.
    if (M >= 16) {
        CHECK_MESSAGE(nontrivial > 0, "expected some partial-OEE exclusions at M=" << M);
    }
}

} // namespace

TEST_CASE("the carried partial-OEE a* matches the GMP reference across survivors") {
    constexpr unsigned A = 6;
    for (const unsigned M : {8u, 12u, 16u}) {
        compare_all(M, A);
    }
#ifdef NDEBUG
    compare_all(20u, A); // heaviest full-sweep case: release only.
#endif
}

#ifdef NDEBUG
// A sampled sweep near production size at M=24, release only: a stride of
// size / 10^5 + 1 (3 for the 211924 survivors) samples about 7*10^4 of them.
TEST_CASE("the carried partial-OEE a* matches the GMP reference at M=24 (sampled)") {
    constexpr unsigned A = 6;
    constexpr unsigned M = 24;
    const std::vector<Node> survs = survivors_with_a_star(M);
    const std::size_t stride = survs.size() > 100000 ? survs.size() / 100000 + 1 : 1;
    const collatz::ref::Options ro = oee_only();
    for (std::size_t i = 0; i < survs.size(); i += stride) {
        const Node& s = survs[i];
        const unsigned got = carried_a_star(s, A);
        const unsigned want = collatz::ref::partial_oee_a_star(s.n0, M, A, ro);
        if (got != want) {
            CAPTURE(collatz::to_string(s.n0));
            CHECK(got == want);
        }
    }
}
#endif
