// Compiles the shared OpenCL/C++ limb header as C++ and checks every primitive
// (and the full 16-step block recurrence) against collatz::u128 over 10^5
// random values. The GPU kernel compiles the same header, so a difference from
// the native reference fails here.
//
// The executable also compiles the cases in test_u128_limbs/: the u96 tail (02),
// the slot loop (03), the mod 2^61-1 folds and digests (04), the q54 max of
// the slot loop and the mod-9 pattern table of the mask (05), and the slot
// loop for n >= 2^64 against GMP (06).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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

constexpr int kIters = 100000;

// Independent native-u128 reference for the 16-step block recurrence, matching
// the u128_limbs.h spec exactly (including overflow + max-block-start tracking).
struct RefResult {
    std::uint64_t blocks;
    u128 max_v;
    bool overflow;
    u128 v_end;
};

[[nodiscard]] RefResult
ref_tail16(u128 n, u128 m_a, const std::array<std::uint32_t, 1u << 16>& tbl) {
    u128 v = m_a;
    u128 maxv = m_a;
    std::uint64_t blocks = 0;
    bool overflow = false;
    while (v >= n) {
        if (blocks >= U128Q_TAIL16_MAX_BLOCKS) {
            overflow = true; // matches the shared recurrence's runaway abort
            break;
        }
        if (v > maxv) {
            maxv = v;
        }
        const auto r = static_cast<std::uint32_t>(collatz::lo64(v) & 0xFFFFu);
        const std::uint32_t e = tbl[r];
        const std::uint32_t c = e >> 5;
        const unsigned f = e & 0x1Fu;
        const u128 h = v >> 16;
        bool o1 = false;
        const u128 prod = collatz::checked_mul(h, collatz::pow3(f), o1);
        bool o2 = false;
        const u128 next = collatz::checked_add(prod, static_cast<u128>(c), o2);
        if (o1 || o2) {
            overflow = true;
            break;
        }
        v = next;
        ++blocks;
    }
    return {blocks, maxv, overflow, v};
}

} // namespace

TEST_CASE("from/to halves round trip") {
    std::mt19937_64 rng(1);
    for (int i = 0; i < kIters; ++i) {
        const u128 x = rand_u128(rng);
        const u128q q = from_native(x);
        CHECK(u128q_lo64(q) == collatz::lo64(x));
        CHECK(u128q_hi64(q) == collatz::hi64(x));
        CHECK(to_native(q) == x);
    }
}

TEST_CASE("u128q_add matches native + and carry") {
    std::mt19937_64 rng(2);
    for (int i = 0; i < kIters; ++i) {
        const u128 a = rand_u128(rng);
        const u128 b = rand_u128(rng);
        u128q_u32 carry = 7;
        const u128q r = u128q_add(from_native(a), from_native(b), &carry);
        bool ovf = false;
        const u128 expected = collatz::checked_add(a, b, ovf);
        CHECK(to_native(r) == expected);
        CHECK((carry != 0) == ovf);
    }
}

TEST_CASE("u128q_add_u32 matches native") {
    std::mt19937_64 rng(3);
    for (int i = 0; i < kIters; ++i) {
        const u128 a = rand_u128(rng);
        const auto k = static_cast<std::uint32_t>(rng());
        u128q_u32 carry = 0;
        const u128q r = u128q_add_u32(from_native(a), k, &carry);
        bool ovf = false;
        const u128 expected = collatz::checked_add(a, static_cast<u128>(k), ovf);
        CHECK(to_native(r) == expected);
        CHECK((carry != 0) == ovf);
    }
}

TEST_CASE("u128q_shift_right_16 matches native >> 16") {
    std::mt19937_64 rng(4);
    for (int i = 0; i < kIters; ++i) {
        const u128 a = rand_u128(rng);
        CHECK(to_native(u128q_shift_right_16(from_native(a))) == (a >> 16));
    }
}

TEST_CASE("u128q_mul_u32 matches native (128x32 -> 128) with overflow") {
    std::mt19937_64 rng(5);
    for (int i = 0; i < kIters; ++i) {
        const u128 a = rand_u128(rng);
        const auto k = static_cast<std::uint32_t>(rng());
        u128q_u32 ovf_q = 0;
        const u128q r = u128q_mul_u32(from_native(a), k, &ovf_q);
        bool ovf = false;
        const u128 expected = collatz::checked_mul(a, static_cast<u128>(k), ovf);
        CHECK(to_native(r) == expected);
        CHECK((ovf_q != 0) == ovf);
    }
}

TEST_CASE("u128q_compare_ge matches native >=") {
    std::mt19937_64 rng(6);
    for (int i = 0; i < kIters; ++i) {
        const u128 a = rand_u128(rng);
        const u128 b = (rng() & 1) ? rand_u128(rng) : a; // sometimes equal
        CHECK((u128q_compare_ge(from_native(a), from_native(b)) != 0) == (a >= b));
    }
}

TEST_CASE("u128q_pow3 matches 3^e for e in [0,16]") {
    u128 p = 1;
    for (std::uint32_t e = 0; e <= 16; ++e) {
        CHECK(static_cast<u128>(u128q_pow3(e)) == p);
        p *= 3;
    }
}

TEST_CASE("u128q_tail16 block recurrence matches native reference") {
    const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16_local();
    std::mt19937_64 rng(7);
    int overflow_hits = 0;
    for (int i = 0; i < kIters; ++i) {
        // n in [2^20, ~2^32], as the tail is driven: n is the start value and the
        // walk from m_a = T^k(n) runs until it drops below n. A large n also keeps
        // the walk off the trivial {1,2} cycle, whose block-start fixed point
        // T^16(2) = 2 never drops below n <= 2; U128Q_TAIL16_MAX_BLOCKS caps that case.
        const u128 n = (rand_u128(rng) >> 96) + (static_cast<u128>(1) << 20);
        u128 m_a = rand_u128(rng); // wide: exercises the overflow path
        if (m_a < n) {
            m_a = n;
        }
        const RefResult ref = ref_tail16(n, m_a, tbl);

        u128q max_q;
        u128q_u32 ovf_q = 0;
        u128q vend_q;
        const u128q_u32 blocks =
            u128q_tail16(from_native(n), from_native(m_a), tbl.data(), &max_q, &ovf_q, &vend_q);

        CHECK(static_cast<std::uint64_t>(blocks) == ref.blocks);
        CHECK((ovf_q != 0) == ref.overflow);
        CHECK(to_native(max_q) == ref.max_v);
        CHECK(to_native(vend_q) == ref.v_end);
        if (ref.overflow) {
            ++overflow_hits;
        }
    }
    // The wide m_a range must have exercised the overflow path at least once.
    CHECK(overflow_hits > 0);
}

TEST_CASE("u128q_tail16 iteration cap aborts the trivial {1,2} cycle") {
    // n = 2, m_a = 2 sits on the block-start fixed point T^16(2) = 2. Both the
    // shared recurrence and the native reference stop at U128Q_TAIL16_MAX_BLOCKS
    // with the overflow flag set.
    const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16_local();
    const u128 n = 2;
    const u128 m_a = 2;
    const RefResult ref = ref_tail16(n, m_a, tbl);
    CHECK(ref.overflow);
    CHECK(ref.blocks == U128Q_TAIL16_MAX_BLOCKS);

    u128q max_q;
    u128q_u32 ovf_q = 0;
    u128q vend_q;
    const u128q_u32 blocks =
        u128q_tail16(from_native(n), from_native(m_a), tbl.data(), &max_q, &ovf_q, &vend_q);
    CHECK(ovf_q != 0);
    CHECK(static_cast<std::uint64_t>(blocks) == ref.blocks);
    CHECK(to_native(max_q) == ref.max_v);
}
