#ifndef COLLATZ_TESTS_FRONTIER_MODEL_RANDOM_BLOCK_HPP
#define COLLATZ_TESTS_FRONTIER_MODEL_RANDOM_BLOCK_HPP

// Random small blocks for the excursion tests (tests/core/test_verify_excursion.cpp
// and tests/integration/test_gpu_excursion.cpp): options that validate_block
// accepts, with value widths from 18 to 96 bits and half of them in the top
// eight bits of that range.

#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdint>
#include <random>
#include <stdexcept>

namespace collatz::frontier_test {

/// Limits of a draw: K in [k_lo, k_hi], A in [a_lo, K - 2] (so M = K - A >= 2),
/// B in [b_lo, 12]. The window templates stay off (each build of them takes a
/// noticeable fraction of a second); the other sieves vary.
struct BlockDraw {
    unsigned k_lo = 6;
    unsigned k_hi = 12;
    unsigned a_lo = 1;
    unsigned b_lo = 2;
};

inline collatz::verify::Options random_block(std::mt19937_64& g, const BlockDraw& d) {
    const auto pick = [&g](unsigned lo, unsigned hi) {
        return lo + static_cast<unsigned>(g() % (hi - lo + 1));
    };
    for (;;) {
        collatz::verify::Options o;
        o.N = pick(d.k_lo, d.k_hi);
        o.A = pick(d.a_lo, o.N - 2);
        o.B = pick(d.b_lo, 12);
        o.num_bitvectors = 8;
        o.threads = 1;
        o.partial_oee = (g() & 1u) != 0;
        o.step1.mod3t = (g() & 2u) != 0 ? 6 : 0;
        o.window_chains = (g() & 4u) != 0 ? 6 : 0;
        o.bv_cache = false;
        // Bit length of H: the block starts at or above 2^17 and ends at or
        // below 2^96.
        const unsigned lo = o.N < 18 ? 18 - o.N : 1;
        const unsigned hi = 96 - o.N;
        const unsigned bits = (g() & 8u) != 0 ? hi - pick(0, 7) : pick(lo, hi);
        const u128 r = make_u128(g(), g());
        o.block_h =
            (static_cast<u128>(1) << (bits - 1)) | (r & ((static_cast<u128>(1) << (bits - 1)) - 1));
        try {
            collatz::verify::validate_block(o);
            return o;
        } catch (const std::invalid_argument&) {
            // the u128 lift bound: draw again
        }
    }
}

} // namespace collatz::frontier_test

#endif // COLLATZ_TESTS_FRONTIER_MODEL_RANDOM_BLOCK_HPP
