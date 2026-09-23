// The u96 block step, the branch-free 3^f and the host flag threshold must be
// bit-identical to the reference forms. The u96 step is u128q_tail16_resume
// from the start (v == maxv, no block done), which is how a tail_persistent
// slot walks a chain that cannot start in its lane loop; u128q_tail16 on the
// host is the u64 + limb loop, which run_old calls, and old_flag is the
// per-candidate flag rule.

#include "collatz/gpu/u128_limbs.h"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "limb_reference.hpp"

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>

using collatz::u128;
using namespace collatz::test::limbs;

namespace {

[[nodiscard]] TailOut run_u96(u128 n, u128 v, const std::uint32_t* tbl) {
    u128q mx;
    u128q ve;
    u128q_u32 ovf = 0;
    const u128q vq = from_native(v);
    const u128q_u32 b = u128q_tail16_resume(from_native(n), vq, vq, 0u, tbl, &mx, &ovf, &ve);
    return {b, to_native(mx), ovf, to_native(ve)};
}

void check_same(u128 n, u128 v, const std::uint32_t* tbl) {
    const TailOut a = run_old(n, v, tbl);
    const TailOut b = run_u96(n, v, tbl);
    const bool same =
        a.blocks == b.blocks && a.max_v == b.max_v && a.ovf == b.ovf && a.v_end == b.v_end;
    if (!same) {
        FAIL_CHECK("u96 differs: v hi=" << collatz::hi64(v) << " lo=" << collatz::lo64(v)
                                        << " n=" << collatz::lo64(n) << " blocks " << a.blocks
                                        << " vs " << b.blocks);
    }
}

// The per-candidate flag rule, with 3^16 = 43046721: ovm || floor(c*3^16 / 2^16) >= r.
[[nodiscard]] bool old_flag(u128q c, u128q r) {
    u128q_u32 ovm = 0;
    const u128q p = u128q_shift_right_16(u128q_mul_u32(c, 43046721u, &ovm));
    return ovm != 0u || u128q_compare_ge(p, r) != 0;
}

} // namespace

TEST_CASE("u128q_pow3_bf equals the 3^e table for every e in [0,16]") {
    for (u128q_u32 e = 0; e <= 16; ++e) {
        CHECK(u128q_pow3_bf(e) == u128q_pow3(e));
        CHECK(static_cast<u128>(u128q_pow3_bf(e)) == collatz::pow3(e));
    }
}

TEST_CASE("u128q_tail16_resume == u128q_tail16 over [2^54, 2^86) and edges") {
    static const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16_local();
    std::mt19937_64 rng(0xE211E211u);
    const u128 one = 1;
    long cases = 0;
    // Every 2^16 residue x 16 random high parts: v in [2^54, 2^86), n < 2^64 at
    // a random width (the tail may cross 2^64 and 2^54 both ways).
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        for (int j = 0; j < 16; ++j) {
            const int vb = 54 + static_cast<int>(rng() % 32); // top bit in [54, 85]
            u128 v = (one << vb) | (collatz::make_u128(rng(), rng()) & ((one << vb) - 1));
            v = (v & ~static_cast<u128>(0xFFFF)) | r;
            const int nb = 1 + static_cast<int>(rng() % 64);
            u128 n = collatz::make_u128(0, rng()) &
                     ((nb == 64) ? ~static_cast<u128>(0) >> 64 : (one << nb) - 1);
            if (n < 3) {
                n = 3; // n <= 2 runs to the 2^20 runaway cap (checked below)
            }
            check_same(n, v, tbl.data());
            ++cases;
        }
    }
    // Edges: 2^54-1, 2^54, 2^64-1, 2^64, 2^86-1, 2^86, 2^96-ish carries,
    // all-ones low words, and each +-2 neighbourhood, against a few n.
    const u128 bases[] = {(one << 54) - 1,
                          one << 54,
                          (one << 64) - 1,
                          one << 64,
                          (one << 86) - 1,
                          one << 86,
                          (one << 86) + 1,
                          (one << 96) - 1,
                          (one << 100) + 12345,
                          collatz::make_u128((1u << 22) - 1, ~0ULL),
                          collatz::make_u128(0x3FFFFF, 0xFFFF000000000000ULL),
                          collatz::make_u128(1, 0),
                          collatz::make_u128(0, ~0ULL << 16)};
    const u128 ns[] = {
        3, 1000, one << 32, (one << 54) - 1, one << 54, (one << 63) + 7, (one << 64) - 1};
    for (const u128 b : bases) {
        for (int d = -2; d <= 2; ++d) {
            const u128 v = b + static_cast<u128>(static_cast<std::int64_t>(d));
            for (const u128 n : ns) {
                check_same(n, v, tbl.data());
                ++cases;
            }
        }
    }
    // n >= 2^64 (limb-only path), v >= n.
    for (int i = 0; i < 2000; ++i) {
        const u128 n = (one << 64) + (collatz::make_u128(rng() % 1000, rng()));
        check_same(n, n + (rng() % (one << 30)), tbl.data());
        ++cases;
    }
    // Random v across the full [0, 2^128) width (limb path, overflow bail).
    for (int i = 0; i < 20000; ++i) {
        check_same(collatz::make_u128(0, rng() | 3u), rand_u128(rng), tbl.data());
        ++cases;
    }
    CHECK(cases >= 1000000);
    // The {1,2} runaway cap (n <= 2): identical abort on both paths.
    for (const u128 v : {static_cast<u128>(2), static_cast<u128>(4), one << 60, one << 70}) {
        for (const u128 n : {static_cast<u128>(1), static_cast<u128>(2)}) {
            check_same(n, v, tbl.data());
        }
    }
}

TEST_CASE("the u96 lane with 2^64 <= n < 2^86 == the limb path (frontier blocks)") {
    // u128q_tail16 here takes the 4-limb step for every n >= 2^64, so it is an
    // independent reference for the u96 lane of u128q_tail16_resume, which also
    // runs for these n.
    static const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16_local();
    std::mt19937_64 rng(0xE238E238u);
    const u128 one = 1;
    // n with its top bit in [64, 85], v from n up to n * 2^24 (a tail may cross
    // 2^86 into the limb path and come back).
    for (int i = 0; i < 200000; ++i) {
        const int nb = 64 + static_cast<int>(rng() % 22);
        const u128 n = (one << nb) | (collatz::make_u128(rng(), rng()) & ((one << nb) - 1));
        const int wb = nb + static_cast<int>(rng() % 25);
        const u128 v = n + (collatz::make_u128(rng(), rng()) & ((one << wb) - 1));
        check_same(n, v, tbl.data());
    }
    // v and n sharing the high word: the hi == n_hi branch of U128Q_U96_GE_N,
    // at v = n - 2 .. n + 2 (0 or more blocks).
    for (int i = 0; i < 20000; ++i) {
        const u128 n = collatz::make_u128(1u + rng() % ((1u << 22) - 1u), rng() | 3u);
        for (int d = -2; d <= 2; ++d) {
            check_same(n, n + static_cast<u128>(static_cast<std::int64_t>(d)), tbl.data());
        }
    }
    // n at the lane's edges: 2^64, 2^86 - 1 (lane) and 2^86 (limb path).
    for (const u128 n : {one << 64, (one << 64) + 1, (one << 86) - 1, one << 86}) {
        for (int i = 0; i < 2000; ++i) {
            check_same(n, n + (collatz::make_u128(0, rng()) >> (rng() % 64)), tbl.data());
        }
    }
    // u128q_tail16_resume from a mid-walk state, n >= 2^64.
    for (int i = 0; i < 20000; ++i) {
        const int nb = 64 + static_cast<int>(rng() % 22);
        const u128 n = (one << nb) | (collatz::make_u128(rng(), rng()) & ((one << nb) - 1));
        const u128 v0 = n + (collatz::make_u128(rng(), rng()) & ((one << (nb + 8)) - 1));
        const TailOut full = run_old(n, v0, tbl.data());
        if (full.ovf != 0u) {
            continue;
        }
        const int j = static_cast<int>(rng() % 6);
        u128 v = v0;
        u128 mx = v0;
        u128q_u32 done = 0;
        while (static_cast<int>(done) < j && v >= n) {
            const std::uint32_t e = tbl[static_cast<std::uint32_t>(collatz::lo64(v)) & 0xFFFFu];
            mx = (v >= mx) ? v : mx;
            v = (v >> 16) * collatz::pow3(e & 0x1Fu) + (e >> 5);
            ++done;
        }
        u128q mq;
        u128q vq;
        u128q_u32 ovf = 0;
        const u128q_u32 b = u128q_tail16_resume(
            from_native(n), from_native(v), from_native(mx), done, tbl.data(), &mq, &ovf, &vq);
        CHECK(b == full.blocks);
        CHECK(to_native(mq) == full.max_v);
        CHECK(ovf == full.ovf);
        CHECK(to_native(vq) == full.v_end);
    }
}

TEST_CASE("u128q_tail16_resume self-check catches an injected step fault") {
    // One table entry the tail hits is corrupted. The u96 path on that table must
    // differ from the u64 + limb loop on the clean one, so the comparison used
    // above is not vacuous.
    static const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16_local();
    std::array<std::uint32_t, 1u << 16> bad = tbl;
    const u128 v = collatz::make_u128(0x1234, 0x9ABCDEF012345677ULL); // ~2^76
    const std::uint32_t r = static_cast<std::uint32_t>(collatz::lo64(v)) & 0xFFFFu;
    bad[r] += 1u << 5; // c + 1
    const TailOut a = run_old(1000, v, tbl.data());
    const TailOut b = run_u96(1000, v, bad.data());
    CHECK((a.max_v != b.max_v || a.v_end != b.v_end || a.blocks != b.blocks));
    const TailOut c = run_u96(1000, v, tbl.data());
    CHECK(a.v_end == c.v_end);
}

TEST_CASE("host flag threshold == old per-candidate flag rule") {
    std::mt19937_64 rng(0x7E3u);
    const u128 one = 1;
    long checked = 0;
    auto check_r = [&](u128 r) {
        const u128q rq = from_native(r);
        const u128q t = u128q_flag_threshold16(rq);
        const u128 tn = to_native(t);
        for (int d = -2; d <= 2; ++d) {
            const u128 c = tn + static_cast<u128>(static_cast<std::int64_t>(d));
            if (d < 0 && tn < static_cast<u128>(-d)) {
                continue;
            }
            const u128q cq = from_native(c);
            CHECK(old_flag(cq, rq) == (u128q_compare_ge(cq, t) != 0));
            ++checked;
        }
        for (int i = 0; i < 4; ++i) {
            const u128q cq = from_native(rand_u128(rng));
            CHECK(old_flag(cq, rq) == (u128q_compare_ge(cq, t) != 0));
            ++checked;
        }
    };
    for (const u128 r : {static_cast<u128>(0),
                         static_cast<u128>(1),
                         static_cast<u128>(43046720),
                         static_cast<u128>(43046721),
                         static_cast<u128>(43046722),
                         one << 16,
                         one << 64,
                         one << 112,
                         one << 116,
                         one << 127,
                         ~static_cast<u128>(0),
                         ~static_cast<u128>(0) - 1}) {
        check_r(r);
    }
    for (int i = 0; i < 200000; ++i) {
        check_r(rand_u128(rng));
    }
    // Real record lower bounds are ~2^70..2^100.
    for (int i = 0; i < 50000; ++i) {
        const int b = 60 + static_cast<int>(rng() % 50);
        check_r((one << b) | (collatz::make_u128(rng(), rng()) & ((one << b) - 1)));
    }
    CHECK(checked > 1000000);
}
