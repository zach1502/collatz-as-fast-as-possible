#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/collatz_fn.hpp"
#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/u128.hpp"

#include <doctest/doctest.h>
#include <random>

using collatz::u128;
using collatz::preimage::ChainEntry;
using collatz::preimage::ChainTable;

namespace {

// Independent brute-force search of the backward preimage tree of a concrete
// value m (depth <= 2t, at most t odd steps), returning the minimal-ratio chain
// that lands below m. Written from the math definition, not from the header's
// explore(), so it is an independent cross-check.
struct BruteChain {
    bool found = false;
    unsigned L = 0;
    unsigned v = 0;
    u128 A = 0;
};

bool better(unsigned l1, unsigned v1, unsigned l2, unsigned v2) {
    const u128 lhs = (static_cast<u128>(1) << l1) * collatz::pow3(v2);
    const u128 rhs = (static_cast<u128>(1) << l2) * collatz::pow3(v1);
    return lhs < rhs;
}

void brute(u128 value, unsigned L, unsigned v, unsigned t, unsigned depth_left, BruteChain& best) {
    if (v >= 1 && (static_cast<u128>(1) << L) < collatz::pow3(v)) {
        const bool take = !best.found || better(L, v, best.L, best.v) ||
                          (L == best.L && v == best.v && value < best.A);
        if (take) {
            best = BruteChain{true, L, v, value};
        }
    }
    if (depth_left == 0) {
        return;
    }
    brute(2 * value, L + 1, v, t, depth_left - 1, best);
    if (v < t && static_cast<unsigned>(value % 3) == 2) {
        brute((2 * value - 1) / 3, L + 1, v + 1, t, depth_left - 1, best);
    }
}

// T applied k times over u128 (the test's values never overflow).
u128 iterate_T(u128 x, unsigned k) {
    for (unsigned i = 0; i < k; ++i) {
        const collatz::StepResult r = collatz::T(x);
        REQUIRE_FALSE(r.overflow);
        x = r.value;
    }
    return x;
}

} // namespace

TEST_CASE("ChainTable entries reproduce a brute-force preimage search on concrete m") {
    std::mt19937_64 rng(0xC0FFEEu);
    for (const unsigned t : {1u, 2u, 3u, 4u}) {
        CAPTURE(t);
        const ChainTable table(t);
        const u128 mod = collatz::pow3(t);
        CHECK(table.modulus() == mod);

        for (u128 c = 0; c < mod; ++c) {
            CAPTURE(collatz::to_string(c));
            const ChainEntry& e = table.entry(c);

            // Structural invariants of a stored chain.
            if (e.has_chain) {
                CHECK(e.v >= 1);
                CHECK(e.v <= t);
                CHECK(e.L <= 2 * t);
                CHECK((static_cast<u128>(1) << e.L) < collatz::pow3(e.v)); // ratio < 1
            }

            // Compare against the brute force on several concrete m == c (mod 3^t).
            for (int trial = 0; trial < 8; ++trial) {
                // m < 3^t * 2^40 keeps every intermediate chain value far below 2^128.
                const u128 r = static_cast<u128>(rng() % (1ull << 40));
                const u128 m = c + mod * r;
                if (m < 2) {
                    continue;
                }
                BruteChain bc;
                brute(m, 0, 0, t, 2 * t, bc);

                CHECK(bc.found == e.has_chain);
                if (!e.has_chain) {
                    continue;
                }
                CHECK(bc.L == e.L);
                CHECK(bc.v == e.v);

                // Reconstruct the ancestor from the stored affine constants and
                // check it matches the brute-force reached value exactly.
                const u128 numer = (m << e.L) - e.K;
                CHECK(numer % collatz::pow3(e.v) == 0); // integral
                const u128 ancestor = numer / collatz::pow3(e.v);
                CHECK(ancestor == bc.A);
                CHECK(ancestor >= 1);
                CHECK(ancestor < m); // ratio < 1 => strictly smaller

                // A is a depth-L T-preimage of m.
                CHECK(iterate_T(ancestor, e.L) == m);
            }
        }
    }
}

TEST_CASE("residues == 0 (mod 3) never have an odd-ancestor chain") {
    // A value == 0 (mod 3) has no odd preimage, and doubling preserves == 0
    // (mod 3), so no value-decreasing chain can exist for it.
    const ChainTable table(3);
    for (u128 c = 0; c < table.modulus(); ++c) {
        if (static_cast<unsigned>(c % 3) == 0) {
            CHECK_FALSE(table.entry(c).has_chain);
        }
    }
}
