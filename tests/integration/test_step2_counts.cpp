#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "scale.hpp"

#include <atomic>
#include <cstdint>
#include <doctest/doctest.h>

using collatz::step1::Node;
using collatz::step2::Filter;
using collatz::step2::Params;
using collatz::test::scaled;

namespace {

// Step-2 candidate count at (N, B) under the four sieves (descent, mod 9,
// path merge, OEE), plus the high-bit partial-OEE prune when `partial_oee` is
// set. B is a parameter so sanitizer builds can use a small look-ahead table
// (the 2^B dip build dominates Filter construction).
std::uint64_t count_candidates(unsigned N, unsigned B, bool partial_oee = false) {
    Params p;
    p.N = N;
    p.A = 6;
    p.B = B;
    p.num_bitvectors = 8;
    p.threads = collatz::hardware_threads();
    p.partial_oee = partial_oee;
    const Filter filter(p);

    std::atomic<std::uint64_t> total{0};
    collatz::step1::Options s1;
    s1.track_oee_astar = true; // Filter::mask reads Node::oee_astar.
    collatz::step1::enumerate_parallel(N - p.A, p.threads, s1, [&](const Node& s) {
        total.fetch_add(static_cast<std::uint64_t>(filter.mask(s).popcount()),
                        std::memory_order_relaxed);
    });
    return total.load();
}

} // namespace

// The Step-2 candidate count is positive and reproducible: the fold does not
// depend on the thread or shard split. A small N keeps the test fast.
TEST_CASE("step2 candidate count is positive and deterministic") {
    const unsigned N = scaled(30, 22);
    const unsigned B = scaled(24, 12);
    CAPTURE(N);
    CAPTURE(B);

    const std::uint64_t got = count_candidates(N, B);
    CHECK(got > 0);
    // A recount must be bit-identical regardless of how work was partitioned.
    CHECK(count_candidates(N, B) == got);
    MESSAGE("N=" << N << " B=" << B << " candidates=" << got);
}

// High-bit partial OEE is deterministic and only removes candidates (it is a
// sound prune), so the pruned count is positive and at most the unpruned one.
TEST_CASE("step2 partial-OEE prune is deterministic and only removes candidates") {
    const unsigned N = scaled(30, 22);
    const unsigned B = scaled(24, 12);
    CAPTURE(N);
    CAPTURE(B);

    const std::uint64_t off = count_candidates(N, B, /*partial_oee=*/false);
    const std::uint64_t on = count_candidates(N, B, /*partial_oee=*/true);
    CHECK(on > 0);
    CHECK(count_candidates(N, B, /*partial_oee=*/true) == on);
    CHECK(on <= off);
    MESSAGE("N=" << N << " B=" << B << " off=" << off << " on=" << on);
}
