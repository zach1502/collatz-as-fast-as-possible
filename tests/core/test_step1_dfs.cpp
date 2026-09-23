// Step 1 (src/core/step1_buffered.cpp, step1_dfs.cpp, step1_path.hpp): the
// buffered enumerator, the u64 DFS against the u128 DFS, and the carried a*.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/collatz_fn.hpp"
#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "partial_oee_walk.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <mutex>
#include <vector>

using collatz::u128;
using collatz::step1::Node;
using collatz::step1::Options;
using collatz::step1::Stats;

TEST_CASE("buffered enumeration yields the same survivor multiset as enumerate_parallel") {
    // enumerate_parallel_buffered delivers the same survivors as the
    // per-survivor enumerate_parallel, in batches. Survivors are unique by n0,
    // so comparing both lists sorted by n0, field by field, checks multiset
    // equality.
    constexpr unsigned M = 24;
    constexpr unsigned threads = 4;
    Options opts; // default options: all sieves on

    std::mutex mtx;
    std::vector<Node> from_parallel;
    collatz::step1::enumerate_parallel(M, threads, opts, [&](const Node& n) {
        const std::lock_guard<std::mutex> lk(mtx);
        from_parallel.push_back(n);
    });

    std::vector<Node> from_buffered;
    const collatz::step1::Stats st = collatz::step1::enumerate_parallel_buffered(
        M, threads, opts, [&](unsigned, const Node* nodes, std::size_t count) {
            const std::lock_guard<std::mutex> lk(mtx);
            for (std::size_t i = 0; i < count; ++i) {
                from_buffered.push_back(nodes[i]);
            }
        });

    CHECK(st.survivors == from_parallel.size());
    REQUIRE(from_buffered.size() == from_parallel.size());

    const auto by_n0 = [](const Node& a, const Node& b) { return a.n0 < b.n0; };
    std::sort(from_parallel.begin(), from_parallel.end(), by_n0);
    std::sort(from_buffered.begin(), from_buffered.end(), by_n0);
    for (std::size_t i = 0; i < from_parallel.size(); ++i) {
        const bool eq = from_buffered[i].n0 == from_parallel[i].n0 &&
                        from_buffered[i].m == from_parallel[i].m &&
                        from_buffered[i].f == from_parallel[i].f &&
                        from_buffered[i].pmax == from_parallel[i].pmax &&
                        from_buffered[i].smax == from_parallel[i].smax;
        CHECK_MESSAGE(eq, "row " << i << " n0=" << collatz::to_string(from_parallel[i].n0));
    }
}

namespace {

std::vector<Node> run(unsigned M, Options o, bool force_u128, unsigned threads, Stats& st) {
    o.force_u128 = force_u128;
    std::vector<Node> out;
    std::mutex mu;
    st = collatz::step1::enumerate_parallel_buffered(
        M, threads, o, [&](unsigned, const Node* nodes, std::size_t n) {
            const std::lock_guard<std::mutex> lk(mu);
            out.insert(out.end(), nodes, nodes + n);
        });
    std::sort(out.begin(), out.end(), [](const Node& a, const Node& b) { return a.n0 < b.n0; });
    return out;
}

void check_same(unsigned M, const Options& o, unsigned threads) {
    Stats s64;
    Stats s128;
    const std::vector<Node> a = run(M, o, false, threads, s64);
    const std::vector<Node> b = run(M, o, true, threads, s128);
    REQUIRE(a.size() == b.size());
    CHECK(s64.survivors == s128.survivors);
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Node& x = a[i];
        const Node& y = b[i];
        const bool same = x.n0 == y.n0 && x.m == y.m && x.f == y.f && x.pmax == y.pmax &&
                          x.smax == y.smax && x.oee_astar == y.oee_astar;
        if (!same) {
            FAIL("node mismatch at index " << i);
        }
    }
}

} // namespace

TEST_CASE("u64 DFS emits the same survivors as the u128 DFS") {
    for (unsigned M : {12u, 20u, 23u}) {
        for (unsigned threads : {1u, 3u}) {
            Options o;
            check_same(M, o, threads);
            o.mod3t = 6;
            o.track_oee_astar = true;
            check_same(M, o, threads);
            o.case_k = 8;
            o.case_c = 5;
            check_same(M, o, threads);
        }
    }
}

TEST_CASE("u64 DFS bound: T^k(n) < 2*3^f for n < 2^k") {
    for (unsigned k = 1; k <= 16; ++k) {
        for (u128 n = 1; n < (static_cast<u128>(1) << k); ++n) {
            u128 v = n;
            unsigned f = 0;
            for (unsigned i = 0; i < k; ++i) {
                f += static_cast<unsigned>(v & 1);
                v = collatz::T(v).value;
            }
            REQUIRE(v < 2 * collatz::pow3(f));
        }
    }
    const u128 two64 = static_cast<u128>(1) << 64;
    CHECK(4 * collatz::pow3(collatz::step1::kU64MaxDepth) < two64);
    CHECK(4 * collatz::pow3(collatz::step1::kU64MaxDepth + 1) >= two64);
}

TEST_CASE("mod-3^t chain constants fit the u64 DFS") {
    for (unsigned t = 1; t <= 8; ++t) {
        const collatz::preimage::ChainTable tbl(t);
        for (u128 r = 0; r < tbl.modulus(); ++r) {
            const collatz::preimage::ChainEntry& e = tbl.entry(r);
            if (e.has_chain) {
                CHECK(e.L >= 1);
                CHECK(e.L <= 2 * t);
                CHECK(e.K < (static_cast<u128>(1) << 62));
            }
        }
    }
}

// The partial-OEE threshold a* carried in the Step-1 DFS
// (step1::Node::oee_astar, enabled by step1::Options::track_oee_astar) must equal the
// OEE prefix walk of tests/support/partial_oee_walk.hpp for every survivor; that
// equality lets Step 2 use the carried value instead of an O(M) prefix walk per
// survivor. The test compares the A-capped result the way Filter::mask() consumes
// it: a* < 2^A prunes, otherwise "no prune" (kNone).

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

// Map a carried u128 oee_astar to the value the walk returns for A: a* < 2^A ->
// a*, else kNone. The no-prune sentinel compares >= 2^A.
std::uint32_t carried_result(u128 oee_astar, unsigned A) {
    const u128 twoA = static_cast<u128>(1) << A;
    if (oee_astar < twoA) {
        return static_cast<std::uint32_t>(oee_astar);
    }
    return kNone;
}

void check_all_survivors(unsigned M, unsigned A) {
    collatz::step1::Options opts;
    opts.track_oee_astar = true;
    std::vector<Node> survs;
    collatz::step1::enumerate(M, opts, [&](const Node& n) { survs.push_back(n); });

    std::uint64_t mismatches = 0;
    for (const Node& s : survs) {
        const std::uint32_t got = carried_result(s.oee_astar, A);
        const std::uint32_t want = collatz::test::partial_oee_walk(s.n0, M, A);
        if (got != want) {
            ++mismatches;
            CAPTURE(M);
            CAPTURE(A);
            CAPTURE(collatz::to_string(s.n0));
            CAPTURE(got);
            CAPTURE(want);
            CHECK(got == want);
            if (mismatches > 20) {
                return;
            }
        }
    }
    CHECK(mismatches == 0);
}

} // namespace

TEST_CASE("DFS-carried oee_astar equals the OEE prefix walk") {
    for (const unsigned M : {16u, 20u, 24u}) {
        for (const unsigned A : {6u, 8u, 10u}) {
            check_all_survivors(M, A);
        }
    }
}

TEST_CASE("track off leaves oee_astar at the no-prune sentinel") {
    collatz::step1::Options opts; // track_oee_astar defaults false
    unsigned checked = 0;
    collatz::step1::enumerate(18u, opts, [&](const Node& n) {
        CHECK(n.oee_astar == collatz::step1::kOeeAStarNone);
        ++checked;
    });
    CHECK(checked > 0);
}
