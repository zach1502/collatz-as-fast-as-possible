// The tail_persistent lane loop for n >= 2^64 (the cpi program without
// COLLATZ_GPU_TAIL_U64N), composed as SLOT_REFILL and SLOT_FIN compose it:
// u128q_lane_fast_ok_n86, u128q_lane_cond_n86 on (n_hi, n_lo), the exit test
// U128Q_U96_GE_N and u128q_tail16_resume with the rebuilt n. Every chain must
// give the GMP walk's blocks, overflow flag and v_end, a max with GMP's
// floor(. / 2^54) that does not exceed it, the n it started from, and the same
// result as the slow lane (u128q_tail16_resume from the start), which runs
// every n >= 2^64 without this lane. The corpus sits at n = 2^86 - 1, 2^86,
// 2^86 + 1 and 2^64 - 1, 2^64, 2^64 + 1, at every residue mod 2^16 next to
// those n and next to v = 2^86, and at random n in [2^64, 2^96).

#include "collatz/gpu/u128_limbs.h"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "lane_n86_reference.hpp"
#include "limb_reference.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <random>

using collatz::u128;
using namespace collatz::test::limbs;

namespace {

struct Tally {
    long cases = 0;
    long lane = 0;
    long bad = 0;
};

// One (n, v): GMP == slow lane exactly, and the lane (stopped after 0, 1, a
// random number of blocks, or not) == GMP up to the q54 max.
void check_chain(GmpWalk& gmp, u128 n, u128 v, std::mt19937_64& rng, Tally& t) {
    const TailOut g = gmp.run(n, v);
    const TailOut s = run_slow(n, v);
    bool ok = s.blocks == g.blocks && s.ovf == g.ovf && s.v_end == g.v_end && s.max_v == g.max_v;
    const u128q_u32 stops[] = {0xFFFFFFFFu, 0u, 1u, static_cast<u128q_u32>(rng() % 8u)};
    bool lane = false;
    for (const u128q_u32 stop : stops) {
        u128 nf = 0;
        const TailOut f = run_lane(n, v, stop, false, &nf, &lane);
        ok = ok && f.blocks == g.blocks && f.ovf == g.ovf && f.v_end == g.v_end && nf == n &&
             f.max_v <= g.max_v && (f.max_v >> 54) == (g.max_v >> 54);
    }
    t.lane += lane ? 1 : 0;
    ++t.cases;
    if (!ok) {
        ++t.bad;
        if (t.bad <= 5) {
            FAIL_CHECK("lane n86 differs: n hi=" << collatz::hi64(n) << " lo=" << collatz::lo64(n)
                                                 << " v hi=" << collatz::hi64(v)
                                                 << " lo=" << collatz::lo64(v));
        }
    }
}

[[nodiscard]] u128 pow2(unsigned k) {
    return static_cast<u128>(1) << k;
}

[[nodiscard]] u128 rand_below(std::mt19937_64& rng, unsigned bits) {
    const u128 x = collatz::make_u128(rng(), rng());
    return bits >= 128u ? x : (x & (pow2(bits) - 1));
}

} // namespace

TEST_CASE("lane n86: the step and q54 bounds of u128q_lane_fast_ok_n86") {
    // c = T^16(r) <= 3^f - 1 for every residue, with equality at r = 2^16 - 1.
    long over = 0;
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        const std::uint32_t f = lane_tbl16()[r] & 0x1Fu;
        over += (lane_tbl16()[r] >> 5) > u128q_pow3(f) - 1u ? 1 : 0;
    }
    CHECK(over == 0);
    CHECK((lane_tbl16()[0xFFFFu] >> 5) == 43046720u);
    // The largest lane start v = 2^86 - 1 (residue 2^16 - 1, f = 16) lands on
    // 2^70 * 3^16 - 1 with hi = 64 * 3^16 - 1 < 2^32, and its q54 fits a uint.
    u128q_u64 lo = ~0ULL;
    u128q_u32 hi = U128Q_U96_HI_LIMIT - 1u;
    u128q_u32 x = 0;
    u128q_u32 blocks = 0;
    CHECK(u128q_x54(lo, hi) == 0xFFFFFFFFu);
    u128q_split_step_maxhi(lane_split8().data(), &lo, &hi, &x, &blocks);
    CHECK(collatz::make_u128(hi, lo) == pow2(70) * collatz::pow3(16) - 1);
    CHECK(hi == 64u * 43046721u - 1u);
    CHECK(x == 0xFFFFFFFFu);
    // The entry test: n <= v < 2^86 exactly.
    const u128 b = pow2(86);
    CHECK(u128q_lane_fast_ok_n86(from_native(b - 1), from_native(b - 1)) != 0);
    CHECK(u128q_lane_fast_ok_n86(from_native(b - 1), from_native(b)) == 0);
    CHECK(u128q_lane_fast_ok_n86(from_native(b), from_native(b)) == 0);
    CHECK(u128q_lane_fast_ok_n86(from_native(b - 1), from_native(b - 2)) == 0);
    CHECK(u128q_lane_fast_ok_n86(from_native(pow2(96) + 5), from_native(b - 1)) == 0);
    CHECK(u128q_lane_fast_ok_n86(from_native(pow2(64)), from_native(pow2(64))) != 0);
    // The {1, 2} cycle (n <= 2) can run into the MAX_BLOCKS cap: the same abort
    // as u128q_tail16 (n = 1 hits it, n = 2 drops below n first).
    int capped = 0;
    for (const u128 v : {static_cast<u128>(2), static_cast<u128>(4), pow2(60), pow2(70)}) {
        for (const u128 n : {static_cast<u128>(1), static_cast<u128>(2)}) {
            const TailOut a = run_old(n, v, lane_tbl16().data());
            u128 nf = 0;
            bool lane = false;
            const TailOut f = run_lane(n, v, 0xFFFFFFFFu, false, &nf, &lane);
            CHECK(lane);
            CHECK((f.blocks == a.blocks && f.ovf == a.ovf && f.v_end == a.v_end));
            capped += (a.ovf == 1u && a.blocks == U128Q_TAIL16_MAX_BLOCKS) ? 1 : 0;
        }
    }
    CHECK(capped >= 4);
}

TEST_CASE("lane n86 == GMP and the slow lane at the n and v bounds, every residue") {
    GmpWalk gmp;
    std::mt19937_64 rng(0x86A86Au);
    Tally t;
    const u128 b86 = pow2(86);
    // n = 2^86 - 1, 2^86, 2^86 + 1 and 2^64 - 1, 2^64, 2^64 + 1, each with
    // v = n - 2^14 .. n + 2^14.
    for (const u128 n0 : {b86, pow2(64)}) {
        for (const u128 n : {n0 - 1, n0, n0 + 1}) {
            for (std::int64_t d = -(1 << 14); d <= (1 << 14); ++d) {
                check_chain(gmp, n, n + static_cast<u128>(d), rng, t);
            }
        }
    }
    // Every residue mod 2^16 of n just below 2^86 and just above 2^64, with
    // v = n, n + 1, 2^86 - 1 (the largest lane start), 2^86 + r (slow lane)
    // and a random v in [n, n * 2^20).
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        for (const u128 n : {b86 - (static_cast<u128>(1) << 16) + r, pow2(64) + r}) {
            const u128 vs[] = {n,
                               n + 1,
                               b86 - 1,
                               b86 + r,
                               n + rand_below(rng, static_cast<unsigned>(64u + rng() % 41u))};
            for (const u128 v : vs) {
                check_chain(gmp, n, v, rng, t);
            }
        }
    }
    // Every residue of v just below 2^86, so the first block from the largest
    // starts takes each (3^f, c), for n near v, mid-range and at 2^64.
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        const u128 v = b86 - (static_cast<u128>(1) << 16) + r;
        for (const u128 n : {v - rand_below(rng, 20), pow2(85) + rand_below(rng, 84), pow2(64)}) {
            check_chain(gmp, n, v, rng, t);
        }
    }
    CHECK(t.bad == 0);
    CHECK(t.cases >= 1000000);
    CHECK(t.lane >= 500000); // the corpus exercises the lane, not only the slow path
}

TEST_CASE("lane n86 == GMP when the first landing value is next to 2^86") {
    // For each residue r with f >= 11 (3^f > 2^16), v0 = q * 2^16 + r < 2^86
    // lands on v1 = q * 3^f + c; q = ceil((2^86 - c) / 3^f) + k puts v1 within
    // 3^f below 2^86 (k < 0), at 2^86 with hi = 2^22 (k = 0) or above (k > 0),
    // so the lane guard and the q54 max are tested at the block after the start.
    GmpWalk gmp;
    std::mt19937_64 rng(0x1A4Du);
    Tally t;
    const u128 b86 = pow2(86);
    long at_bound = 0;
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        const std::uint32_t f = lane_tbl16()[r] & 0x1Fu;
        if (f < 11u) {
            continue;
        }
        const u128 k3 = u128q_pow3(f);
        const u128 c = lane_tbl16()[r] >> 5;
        const u128 q0 = (b86 - c + k3 - 1) / k3;
        for (int k = -2; k <= 2; ++k) {
            const u128 q = q0 + static_cast<u128>(static_cast<std::int64_t>(k));
            const u128 v0 = (q << 16) | r;
            const u128 v1 = q * k3 + c;
            REQUIRE(v0 < b86);
            at_bound += (v1 >= b86 && (v1 >> 64) == U128Q_U96_HI_LIMIT) ? 1 : 0;
            for (const u128 n : {v0, v0 - rand_below(rng, 40), pow2(64) + rand_below(rng, 60)}) {
                check_chain(gmp, n, v0, rng, t);
            }
        }
    }
    CHECK(t.bad == 0);
    CHECK(at_bound > 1000);
    CHECK(t.lane == t.cases);
}

TEST_CASE("lane n86 == GMP and the slow lane at random n in [2^64, 2^96)") {
    GmpWalk gmp;
    std::mt19937_64 rng(0x6496u);
    Tally t;
    for (int i = 0; i < 300000; ++i) {
        const unsigned nb = 64u + static_cast<unsigned>(rng() % 32u); // top bit of n
        const u128 n = pow2(nb) | rand_below(rng, nb);
        const unsigned wb = nb + static_cast<unsigned>(rng() % 28u); // v up to ~n * 2^27
        const u128 v = (rng() % 16u == 0u) ? n - rand_below(rng, 8) : n + rand_below(rng, wb);
        check_chain(gmp, n, v, rng, t);
    }
    CHECK(t.bad == 0);
    CHECK(t.lane >= 50000);
}

TEST_CASE("lane n86: the corpus sees the n < 2^64 exit rule applied to n >= 2^64") {
    // The lane loop with u128q_lane_cond and the hi != 0 exit test, which ignore
    // n_hi: it must differ from GMP on lane chains like those above.
    GmpWalk gmp;
    std::mt19937_64 rng(0xBADu);
    long differ = 0;
    for (int i = 0; i < 2000; ++i) {
        const u128 n = pow2(80) + rand_below(rng, 80);
        const u128 v = n + rand_below(rng, 40);
        const TailOut g = gmp.run(n, v);
        u128 nf = 0;
        bool lane = false;
        const TailOut f = run_lane(n, v, 0xFFFFFFFFu, true, &nf, &lane);
        differ += (f.blocks != g.blocks || f.v_end != g.v_end) ? 1 : 0;
    }
    CHECK(differ > 0);
}
