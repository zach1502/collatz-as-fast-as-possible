// The tail_persistent slot loop as the kernel runs it: u128q_lane_fast_ok and
// u128q_lane_cond, u128q_split_step_maxhi on the split-8 {3^f, c} sub-tables,
// which keeps only floor(max / 2^54) of the block-start max, then
// u128q_tail16_resume. It must reproduce u128q_tail16's blocks, overflow flag
// and v_end, and give a max with the same floor(. / 2^54) that does not exceed
// the true one. The u64-n fold helpers must equal the u128q folds.

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
using namespace collatz::test::limbs;

namespace {

[[nodiscard]] const std::array<std::uint32_t, 1u << 16>& tbl16() {
    static const std::array<std::uint32_t, 1u << 16> t = build_tbl16_local();
    return t;
}

// The split-8 image the host fills and each work-group copies to __local: the
// 256 {3^f, c} pairs of the 8-step map, applied twice per block.
[[nodiscard]] const std::vector<std::uint32_t>& split8() {
    static const std::vector<std::uint32_t> img = [] {
        std::vector<std::uint32_t> v(2u * u128q_split_entries());
        u128q_split_fill(v.data());
        return v;
    }();
    return img;
}

// One chain as a tail_persistent slot walks it. The kernel leaves the lane loop
// only when u128q_lane_cond fails; `stop` also leaves it after that many
// blocks, so the resume runs from every lane-loop state.
[[nodiscard]] TailOut run_slot(u128 n, u128 v, const std::uint32_t* split_tab, u128q_u32 stop) {
    const u128q nq = from_native(n);
    const u128q vq = from_native(v);
    u128q mx;
    u128q ve;
    u128q_u32 ovf = 0;
    if (!u128q_lane_fast_ok(nq, vq)) {
        const u128q_u32 b = u128q_tail16_resume(nq, vq, vq, 0u, tbl16().data(), &mx, &ovf, &ve);
        return {b, to_native(mx), ovf, to_native(ve)};
    }
    const u128q_u64 n_lo = u128q_lo64(nq);
    u128q_u64 lo = u128q_lo64(vq);
    u128q_u32 hi = vq.w[2];
    u128q_u32 x = u128q_x54(lo, hi);
    u128q_u32 blocks = 0;
    while (blocks < stop && u128q_lane_cond(lo, hi, n_lo, blocks)) {
        u128q_split_step_maxhi(split_tab, &lo, &hi, &x, &blocks);
    }
    mx = u128q_from_x54(x);
    ve = u128q_from_halves(lo, hi);
    if (hi != 0u || lo >= n_lo) {
        blocks = u128q_tail16_resume(
            u128q_from_halves(n_lo, 0), ve, mx, blocks, tbl16().data(), &mx, &ovf, &ve);
    }
    return {blocks, to_native(mx), ovf, to_native(ve)};
}

// The slot without a stop and stopped after 0, 1 and a random number of blocks.
// The kernel's flag test compares floor(max / 2^54) with that of the threshold,
// and the host takes the max as a lower bound, so the slot's max must have the
// true max's floor(. / 2^54) and must not exceed it.
void check_slot(u128 n, u128 v, std::mt19937_64& rng, long& cases) {
    const TailOut a = run_old(n, v, tbl16().data());
    const u128q_u32 stops[] = {0xFFFFFFFFu, 0u, 1u, static_cast<u128q_u32>(rng() % 12u)};
    for (const u128q_u32 stop : stops) {
        const TailOut b = run_slot(n, v, split8().data(), stop);
        const bool same = a.blocks == b.blocks && a.ovf == b.ovf && a.v_end == b.v_end &&
                          b.max_v <= a.max_v && (b.max_v >> 54) == (a.max_v >> 54);
        if (!same) {
            FAIL_CHECK("slot stop=" << stop << " differs: v hi=" << collatz::hi64(v)
                                    << " lo=" << collatz::lo64(v) << " n=" << collatz::lo64(n)
                                    << " blocks " << a.blocks << " vs " << b.blocks);
        }
        ++cases;
    }
}

} // namespace

TEST_CASE("split-8 sub-table composition == TBL16 {3^f, c} on all 2^16 residues") {
    const std::array<std::uint32_t, 1u << 16>& t = tbl16();
    CHECK(8u * u128q_split_entries() == 2048u);
    long bad = 0;
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        u128q_u64 k = 0;
        u128q_u64 c = 0;
        u128q_split_kc(split8().data(), r, &k, &c);
        const bool same =
            k == static_cast<u128q_u64>(collatz::pow3(t[r] & 0x1Fu)) && c == (t[r] >> 5);
        bad += same ? 0 : 1;
    }
    CHECK(bad == 0);
}

TEST_CASE("slot loop: lane loop + resume == u128q_tail16 at every stop point") {
    std::mt19937_64 rng(0xE213E213u);
    const u128 one = 1;
    long cases = 0;
    // Every residue x 8 random v in [2^40, 2^90), n < 2^64 at a random width:
    // crosses 2^54 / 2^64 / 2^86 in both directions.
    for (std::uint32_t r = 0; r < (1u << 16); ++r) {
        for (int j = 0; j < 8; ++j) {
            const int vb = 40 + static_cast<int>(rng() % 50);
            u128 v = (one << vb) | (collatz::make_u128(rng(), rng()) & ((one << vb) - 1));
            v = (v & ~static_cast<u128>(0xFFFF)) | r;
            const int nb = 1 + static_cast<int>(rng() % 64);
            u128 n = collatz::make_u128(0, rng()) &
                     ((nb == 64) ? ~static_cast<u128>(0) >> 64 : (one << nb) - 1);
            if (n < 3) {
                n = 3;
            }
            check_slot(n, v, rng, cases);
        }
    }
    // The lane-loop guard edges (2^86 +-2, hi = 2^22 - 1 carries, v == n,
    // v = n - 1) and the edge set of the u96 test, against a few n.
    const u128 bases[] = {(one << 54) - 1,
                          one << 64,
                          (one << 86) - 2,
                          (one << 86) - 1,
                          one << 86,
                          (one << 86) + 1,
                          (one << 96) - 1,
                          collatz::make_u128((1u << 22) - 1, ~0ULL),
                          collatz::make_u128(0x3FFFFF, 0xFFFF000000000000ULL),
                          collatz::make_u128(0, ~0ULL << 16)};
    const u128 ns[] = {3, 1000, one << 32, (one << 54) - 1, (one << 63) + 7, (one << 64) - 1};
    for (const u128 b : bases) {
        for (int d = -2; d <= 2; ++d) {
            const u128 v = b + static_cast<u128>(static_cast<std::int64_t>(d));
            for (const u128 n : ns) {
                check_slot(n, v, rng, cases);
            }
        }
    }
    for (const u128 n : ns) {
        check_slot(n, n, rng, cases);     // v == n: at least one block
        check_slot(n, n - 1, rng, cases); // v < n: zero blocks
    }
    // n >= 2^64 (never enters the lane loop) and full-width v (limb path,
    // overflow bail).
    for (int i = 0; i < 2000; ++i) {
        const u128 n = (one << 64) + (collatz::make_u128(rng() % 1000, rng()));
        check_slot(n, n + (rng() % (one << 30)), rng, cases);
    }
    for (int i = 0; i < 20000; ++i) {
        check_slot(collatz::make_u128(0, rng() | 3u), rand_u128(rng), rng, cases);
    }
    // The {1,2} runaway cap (n <= 2) aborts identically.
    for (const u128 v : {static_cast<u128>(2), static_cast<u128>(4), one << 60, one << 70}) {
        for (const u128 n : {static_cast<u128>(1), static_cast<u128>(2)}) {
            check_slot(n, v, rng, cases);
        }
    }
    CHECK(cases >= 2000000);
}

TEST_CASE("slot loop: u128q_tail16_resume continues a walk stopped at any block") {
    // Walks j blocks natively (block-start max included), resumes, and compares
    // with the uninterrupted tail, including limb-path (>= 2^86) mid states.
    const std::array<std::uint32_t, 1u << 16>& t = tbl16();
    std::mt19937_64 rng(0x5E5u);
    const u128 one = 1;
    for (int i = 0; i < 50000; ++i) {
        const int vb = 30 + static_cast<int>(rng() % 90);
        const u128 v0 = (one << vb) | (collatz::make_u128(rng(), rng()) & ((one << vb) - 1));
        const u128 n = collatz::make_u128(0, rng() >> (rng() % 60)) | 3u;
        const TailOut full = run_old(n, v0, t.data());
        const int j = static_cast<int>(rng() % 8);
        u128 v = v0;
        u128 mx = v0;
        u128q_u32 done = 0;
        bool ok = true;
        while (static_cast<int>(done) < j && v >= n) {
            const std::uint32_t e = t[static_cast<std::uint32_t>(collatz::lo64(v)) & 0xFFFFu];
            const u128 h = v >> 16;
            const u128 k = collatz::pow3(e & 0x1Fu);
            if (h != 0 && k > (~static_cast<u128>(0) - (e >> 5)) / h) {
                ok = false; // this walk overflows u128; covered by the full-width case above
                break;
            }
            mx = (v >= mx) ? v : mx;
            v = h * k + (e >> 5);
            ++done;
        }
        if (!ok) {
            continue;
        }
        u128q mq;
        u128q vq;
        u128q_u32 ovf = 0;
        const u128q_u32 b = u128q_tail16_resume(
            from_native(n), from_native(v), from_native(mx), done, t.data(), &mq, &ovf, &vq);
        CHECK(b == full.blocks);
        CHECK(to_native(mq) == full.max_v);
        CHECK(ovf == full.ovf);
        CHECK(to_native(vq) == full.v_end);
    }
}

TEST_CASE("slot loop: the lane loop is not vacuous (a corrupted split entry is seen)") {
    std::vector<std::uint32_t> bad = split8();
    const u128 v = collatz::make_u128(0x1234, 0x9ABCDEF012345677ULL); // ~2^76, lane loop
    const std::uint32_t i = static_cast<std::uint32_t>(collatz::lo64(v)) & 0xFFu;
    bad[2u * i] += 1u; // 3^f + 1 of the first 8 steps of the first block
    const TailOut a = run_old(1000, v, tbl16().data());
    const TailOut b = run_slot(1000, v, bad.data(), 0xFFFFFFFFu);
    CHECK((a.v_end != b.v_end || a.blocks != b.blocks || (a.max_v >> 54) != (b.max_v >> 54)));
    CHECK(run_slot(1000, v, split8().data(), 0xFFFFFFFFu).v_end == a.v_end);
}

TEST_CASE("u64-n coverage key and digest term == the u128q functions") {
    std::mt19937_64 rng(0x64u);
    for (int i = 0; i < 200000; ++i) {
        const u128q_u64 n = rng() >> (rng() % 64);
        const u128q nq = u128q_from_halves(n, 0);
        const u128q ve = from_native(rand_u128(rng));
        const u128q_u64 blocks = rng() % 5000u;
        CHECK(u128q_cov_key_n64(n) == u128q_cov_key(nq));
        CHECK(u128q_result_digest_term_n64(n, blocks, ve) ==
              u128q_result_digest_term(nq, blocks, ve));
    }
}
