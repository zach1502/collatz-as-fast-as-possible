// Step 2 dip builders: the residue-local premise of every window gain table, and
// byte-identical cache files from two fresh builds of each config.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "builder_equiv.hpp"
#include "collatz/preimage_chain.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>

// The class-major dip builder walks a shared prefix once per class, so it
// assumes the window gain gain(c, o) depends only on c mod 3^o (the argument is
// written at WindowChainTable::residue_local). The first test checks that
// premise for every table the Filter can build (chain tables t_w = 1..max_t with
// BFS depth 2 t_w, and template tables D = 1..max_window_t with t = min(D, 12) and
// depth D), exhaustively over each table's domain, against an independent brute
// force on the integers c + k 3^o. The second checks that two fresh builds write
// byte-identical dip and bv cache files at small B, on both sides of B = 14,
// where the class-major builder takes over from the per-residue one.

using namespace collatz::test::builder_equiv;

TEST_CASE("window gain premise: residue-local for every chain t_w and template D") {
#ifdef NDEBUG
    const unsigned kMaxT = collatz::preimage::max_t;
    const unsigned kMaxD = collatz::preimage::max_window_t;
#else
    const unsigned kMaxT = 8; // debug + ASan: the t=9/10, D=10..12 BFS is slow.
    const unsigned kMaxD = 9;
#endif
    std::uint64_t n = 0;
    for (unsigned t = 1; t <= kMaxT; ++t) {
        CAPTURE(t);
        n += check_premise(t, 2u * t, t <= 6u ? 1u : 97u);
    }
    for (unsigned D = 1; D <= kMaxD; ++D) {
        CAPTURE(D);
        n += check_premise(std::min(D, collatz::preimage::max_window_t), D, D <= 7u ? 1u : 331u);
    }
    MESSAGE("brute-force comparisons: " << n);
}

TEST_CASE("dip builders write the same cache files twice at small B, below and above B=14") {
#ifdef NDEBUG
    const unsigned kBs[] = {8, 10, 12, 13, 14, 16};
    const Cfg kWide[] = {{13, 10, 0}, {14, 6, 12}, {16, 10, 0}};
#else
    const unsigned kBs[] = {8, 10, 13, 14};
    const Cfg kWide[] = {{14, 6, 6}};
#endif
    for (const unsigned B : kBs) {
        for (const Cfg c : {Cfg{B, 0, 0}, Cfg{B, 1, 0}, Cfg{B, 3, 5}, Cfg{B, 0, 7}}) {
            check_builders(c);
        }
    }
    for (const Cfg c : kWide) {
        check_builders(c);
    }
}
