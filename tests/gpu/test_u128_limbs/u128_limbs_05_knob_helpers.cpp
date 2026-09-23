// The u128_limbs.h helpers behind the q54 block-start max of the tail slot loop
// (u128q_split_step_maxhi, u128q_x54, u128q_from_x54, u128q_ge_q54), the
// __constant mod-9 pattern table of the mask kernel and its start residue
// u128q_mod9.

#include "collatz/gpu/u128_limbs.h"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "limb_reference.hpp"

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <vector>

using collatz::u128;

namespace {

// q(v) of a 96-bit (hi:lo) value, written independently of the kernel helper.
std::uint64_t q54(std::uint64_t lo, std::uint64_t hi) {
    return (lo >> 54) | (hi << 10);
}

} // namespace

TEST_CASE("maxhi step == the native 16-step jump on (lo, hi, blocks) and keeps q(max)") {
    std::vector<u128q_u32> tab(2u * u128q_split_entries());
    u128q_split_fill(tab.data());
    const std::array<std::uint32_t, 1u << 16> tbl = collatz::test::limbs::build_tbl16_local();
    std::mt19937_64 rng(0xE237u + 8u);
    for (int trial = 0; trial < 20000; ++trial) {
        // Lane-loop states: v < 2^86 (hi < 2^22), a spread of magnitudes.
        const unsigned bits = 40u + static_cast<unsigned>(rng() % 46u);
        std::uint64_t lo = rng();
        std::uint32_t hi = 0;
        if (bits > 64) {
            hi = static_cast<std::uint32_t>(rng() & ((std::uint64_t{1} << (bits - 64)) - 1u));
        } else {
            lo &= (bits == 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1u);
        }
        // Reference: v' = (v >> 16) * 3^f + c from TBL16 in u128, and the exact
        // block-start max.
        u128 v = collatz::make_u128(hi, lo);
        u128 vmax = v;
        std::uint64_t b_lo = lo;
        std::uint32_t b_hi = hi;
        std::uint32_t mx = u128q_x54(lo, hi);
        std::uint32_t b_bl = 0;
        REQUIRE(mx == q54(lo, hi));
        for (std::uint32_t step = 0; step < 6 && b_hi < (1u << 22); ++step) {
            const std::uint32_t e = tbl[static_cast<std::uint32_t>(collatz::lo64(v)) & 0xFFFFu];
            vmax = v > vmax ? v : vmax;
            v = (v >> 16) * collatz::pow3(e & 0x1Fu) + (e >> 5);
            u128q_split_step_maxhi(tab.data(), &b_lo, &b_hi, &mx, &b_bl);
            REQUIRE(collatz::make_u128(b_hi, b_lo) == v);
            REQUIRE(b_bl == step + 1u);
            // The exact max's q() equals the u32 kept by the maxhi step.
            REQUIRE(static_cast<u128>(mx) == (vmax >> 54));
        }
        // FIN's lower bound: L0 = mx * 2^54 <= max, q(L0) = mx.
        const u128 l0 = collatz::test::limbs::to_native(u128q_from_x54(mx));
        CHECK(l0 <= vmax);
        CHECK((l0 >> 54) == static_cast<u128>(mx));
    }
}

TEST_CASE("q54 max: the flag test is a superset of the exact threshold rule") {
    const auto to_q = [](u128 v) {
        return u128q_from_halves(static_cast<std::uint64_t>(v),
                                 static_cast<std::uint64_t>(v >> 64));
    };
    std::mt19937_64 rng(0xF1A6u);
    std::uint64_t exact = 0;
    std::uint64_t super = 0;
    for (int i = 0; i < 300000; ++i) {
        // thr across the whole u128 range, T near it (both sides).
        const unsigned tb = 1u + static_cast<unsigned>(rng() % 127u);
        const u128 r128 = (static_cast<u128>(rng()) << 64) | rng();
        const u128 thr = r128 >> (128u - tb);
        const u128 d = static_cast<u128>(rng() % (std::uint64_t{1} << (rng() % 60u)));
        u128 t = thr;
        if ((rng() & 1u) != 0u) {
            t = (thr + d < thr) ? thr : thr + d;
        } else {
            t = thr >= d ? thr - d : 0;
        }
        // A max that went through the maxhi lane loop is floored to a multiple of 2^54.
        const u128 mx = (rng() & 1u) != 0u ? ((t >> 54) << 54) : t;
        REQUIRE(mx <= t);
        const u128 qthr = thr >> 54;
        const int fl_exact = t >= thr ? 1 : 0;
        const int fl_q = u128q_ge_q54(
            to_q(mx), static_cast<std::uint64_t>(qthr), static_cast<std::uint64_t>(qthr >> 64));
        CHECK(fl_q == (((mx >> 54) >= qthr) ? 1 : 0)); // helper == its definition
        exact += static_cast<std::uint64_t>(fl_exact);
        super += static_cast<std::uint64_t>(fl_q);
        if (fl_exact != 0) {
            REQUIRE(fl_q != 0);
        }
    }
    CHECK(super >= exact);
}

TEST_CASE("the __constant mod-9 pattern table: bit j of (p, r) keeps (r + j*p) mod 9") {
    // The mask keeps candidate a of a lane whose start residue is r and
    // p = 2^M mod 9 unless (r + j*p) mod 9 is 2, 4, 5 or 8, with j = a mod 32.
    for (std::uint32_t p = 0; p < 9u; ++p) {
        for (std::uint32_t r = 0; r < 9u; ++r) {
            std::uint32_t want = 0;
            for (std::uint32_t j = 0; j < 32u; ++j) {
                const std::uint32_t res = (r + j * p) % 9u;
                const bool drop = res == 2u || res == 4u || res == 5u || res == 8u;
                want |= (drop ? 0u : 1u) << j;
            }
            INFO("p=" << p << " r=" << r);
            CHECK(u128q_mod9_pat[p * 9u + r] == want);
        }
    }
}

TEST_CASE("u128q_mod9 == n mod 9 (the mask's start residue)") {
    using collatz::test::limbs::from_native;
    std::mt19937_64 rng(0x9A9u);
    long bad = 0;
    const auto check = [&](u128 n) {
        bad += u128q_mod9(from_native(n)) == static_cast<u128q_u32>(n % 9u) ? 0 : 1;
    };
    for (int i = 0; i < 1000000; ++i) {
        check(collatz::test::limbs::rand_u128(rng));
    }
    // 2^k + d across every limb edge, 0 and 2^128 - 1.
    for (unsigned k = 0; k < 128u; ++k) {
        for (std::int64_t d = -20; d <= 20; ++d) {
            check((static_cast<u128>(1) << k) + static_cast<u128>(d));
        }
    }
    // Every combination of limb residues, each limb near 0 or near 2^32.
    for (std::uint32_t c = 0; c < 6561u; ++c) {
        u128q q;
        std::uint32_t x = c;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t r = x % 9u;
            x /= 9u;
            q.w[i] = (rng() & 1u) != 0
                         ? 0xFFFFFFFCu - 9u * static_cast<std::uint32_t>(1u + rng() % 3u) + r
                         : r + 9u * static_cast<std::uint32_t>(rng() % 1000u);
        }
        check(collatz::test::limbs::to_native(q));
    }
    CHECK(bad == 0);
}
