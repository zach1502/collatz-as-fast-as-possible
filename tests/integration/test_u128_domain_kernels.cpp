// The cases for (1), (2, observable), (3) and (4) of the u128 domain tests.
// test_u128_domain.cpp, the other source of this executable, lists the scope
// and defines main.

#include "collatz/collatz_fn.hpp"
#include "collatz/gpu/u128_limbs.h"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step3.hpp"
#include "scale.hpp"
#include "step1_gmp.hpp"
#include "u128_domain.hpp"

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <vector>

using collatz::u128;
namespace s3 = collatz::step3;
using namespace collatz::test::u128_domain;

namespace {

// Locally-built 16-step packed table (c << 5) | f, matching the kernels.
std::array<std::uint32_t, 1u << 16> build_tbl16() {
    std::array<std::uint32_t, 1u << 16> tbl{};
    for (std::size_t r = 0; r < tbl.size(); ++r) {
        u128 vv = static_cast<u128>(r);
        unsigned f = 0;
        for (unsigned j = 0; j < 16; ++j) {
            if ((vv & 1) != 0) {
                ++f;
            }
            vv = collatz::T(vv).value;
        }
        tbl[r] = (static_cast<std::uint32_t>(vv) << 5) | static_cast<std::uint32_t>(f);
    }
    return tbl;
}

struct NM {
    u128 n;
    u128 m;
};

// Adversarial (n, m = T^k(n)) pairs: the boundary points and random n, each
// advanced k steps in GMP; only pairs with m >= n (the Step-3 precondition)
// are kept.
std::vector<NM> make_nm_pairs(std::mt19937_64& rng, int count) {
    std::vector<NM> out;
    out.reserve(static_cast<std::size_t>(count));
    const std::vector<u128> fixed = adversarial_points();
    while (static_cast<int>(out.size()) < count) {
        u128 n;
        if (out.size() < fixed.size()) {
            n = fixed[out.size()];
        } else {
            // n spanning 2^20 .. 2^127, biased across the range.
            const int bits = 20 + static_cast<int>(rng() % 108); // [20, 127]
            n = collatz::make_u128(rng(), rng());
            n &= (static_cast<u128>(1) << bits) - 1;
            n |= static_cast<u128>(1) << (bits - 1); // n has exactly `bits` bits
        }
        if (n < 3) {
            n = 3;
        }
        const unsigned k = static_cast<unsigned>(rng() % 48); // k in [0, 47]
        const mpz_class m_mpz = collatz::test::iterate_T(u128_to_mpz(n), k);
        if (m_mpz < u128_to_mpz(n)) {
            continue; // need m >= n
        }
        if (mpz_sizeinbase(m_mpz.get_mpz_t(), 2) > 128) {
            continue; // keep m in-domain; the finisher takes u128
        }
        out.push_back(NM{n, mpz_to_u128(m_mpz)});
    }
    return out;
}

} // namespace

// (1) + (4) block recurrence vs GMP on adversarial pairs.
TEST_CASE("block + exact kernels agree with GMP across the u128 domain") {
    std::mt19937_64 rng(0x128D0Au);
    const int kSamples = static_cast<int>(collatz::test::scaled(100000, 1200));
    const std::vector<NM> pairs = make_nm_pairs(rng, kSamples);

    int overflow_hits = 0;
    int large_n = 0; // n > 2^64
    for (const NM& p : pairs) {
        const u128 n = p.n;
        const u128 m = p.m;
        if (collatz::hi64(n) != 0) {
            ++large_n;
        }
        CAPTURE(collatz::to_string(n));
        CAPTURE(collatz::to_string(m));

        // 16-step block recurrence: fast u128 vs GMP replay vs ref/ GMP.
        const s3::BlockOutcome fast = s3::finish_block16(n, m, 0);
        const s3::BlockOutcome ora = s3::finish_block16_oracle(n, m);
        const collatz::ref::BlockResult refb = collatz::ref::finish_blocks(n, m);
        // The two independent GMP block replays must always agree.
        CHECK(ora.blocks == refb.blocks);
        CHECK(ora.v_end == refb.v_end);
        if (fast.overflow) {
            ++overflow_hits;
            // Contract: on u128 overflow the fast kernel bails; the GMP replay
            // reproduces the same block count and landing value exactly.
            CHECK(ora.blocks == refb.blocks);
            CHECK(ora.v_end == refb.v_end);
        } else {
            CHECK(fast.blocks == ora.blocks);
            CHECK(fast.v_end == ora.v_end);
            // threshold 0 => exact interior max; the GMP oracle single-steps
            // every block, so its max is exact too.
            CHECK(fast.max_seen == ora.max_seen);
        }
    }
    // The adversarial spread must have exercised both the overflow (past-2^128)
    // path and starting values above 2^64.
    CHECK(overflow_hits > 0);
    CHECK(large_n > 0);
}

// (3) GPU 4x32-limb block recurrence (host-compiled) vs u128 vs GMP.
TEST_CASE("GPU limb recurrence matches u128 and GMP across the domain") {
    const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16();
    std::mt19937_64 rng(0x6907Au);
    const int kSamples = static_cast<int>(collatz::test::scaled(60000, 1000));
    const std::vector<NM> pairs = make_nm_pairs(rng, kSamples);

    const auto to_q = [](u128 x) { return u128q_from_halves(collatz::lo64(x), collatz::hi64(x)); };
    const auto to_native = [](u128q q) { return collatz::make_u128(u128q_hi64(q), u128q_lo64(q)); };

    int gpu_overflow = 0;
    for (const NM& p : pairs) {
        const u128 n = p.n;
        const u128 m = p.m;
        if (n <= 2) {
            continue; // the block kernels special-case n<=2; the limb loop caps.
        }
        CAPTURE(collatz::to_string(n));
        CAPTURE(collatz::to_string(m));

        u128q max_q;
        u128q_u32 ovf_q = 0;
        u128q vend_q;
        const u128q_u32 blocks =
            u128q_tail16(to_q(n), to_q(m), tbl.data(), &max_q, &ovf_q, &vend_q);

        // The pure block recurrence (blocks + landing value) is the GMP replay;
        // finish_block16(.,.,0) also reconstructs the exact interior max and so
        // reports overflow whenever that max exceeds 2^128 even though the block
        // recurrence itself is finite, so it is not the right oracle for the
        // limb loop. Compare against the GMP block recurrence instead, and only
        // cross-check the u128 kernel's block count on a high threshold (which
        // disables the exact-max path) when it does not overflow.
        const collatz::ref::BlockResult refb = collatz::ref::finish_blocks(n, m);
        const s3::BlockOutcome fast = s3::finish_block16(n, m, collatz::u128_max);

        if (ovf_q != 0) {
            ++gpu_overflow;
            // GPU bailed (true 2^128 block-landing overflow or the MAX_BLOCKS
            // runaway cap). Either the u128 kernel also bailed on the landing, or
            // the block count hit the cap.
            CHECK((fast.overflow || blocks == U128Q_TAIL16_MAX_BLOCKS));
        } else {
            // No overflow: block count and landing value match GMP exactly.
            CHECK(static_cast<std::uint64_t>(blocks) == refb.blocks);
            CHECK(to_native(vend_q) == refb.v_end);
            if (!fast.overflow) {
                CHECK(fast.blocks == static_cast<std::uint64_t>(blocks));
                CHECK(fast.v_end == to_native(vend_q));
            }
        }
    }
    CHECK(gpu_overflow > 0);
}

// MAX_BLOCKS cap semantics: a degenerate trivial-cycle input (n=2, m=2) would
// spin the block-start fixed point T^16(2)=2 forever; the shared recurrence must
// abort at U128Q_TAIL16_MAX_BLOCKS with the overflow/abort flag set.
TEST_CASE("GPU limb recurrence caps the trivial {1,2} cycle") {
    const std::array<std::uint32_t, 1u << 16> tbl = build_tbl16();
    const auto to_q = [](u128 x) { return u128q_from_halves(collatz::lo64(x), collatz::hi64(x)); };
    u128q max_q;
    u128q_u32 ovf_q = 0;
    u128q vend_q;
    const u128q_u32 blocks = u128q_tail16(to_q(2), to_q(2), tbl.data(), &max_q, &ovf_q, &vend_q);
    CHECK(ovf_q != 0);
    CHECK(static_cast<std::uint64_t>(blocks) == U128Q_TAIL16_MAX_BLOCKS);
}

// (2, observable) Overflow -> GMP fallback stays exact and flags excursions
// that exceed 2^128, for starting values above 2^64 up to 2^127.
TEST_CASE("overflow fallback: oracle exact + max_exceeds_u128 for n above 2^64") {
    // 2^127 - 1 has an excursion far beyond 2^128; finish_block16 bails and the
    // oracle, which has no overflow outcome, finishes exactly with the excursion
    // flagged.
    const u128 big = (static_cast<u128>(1) << 127) - 1;
    const s3::BlockOutcome fastb = s3::finish_block16(big, big, 0);
    CHECK(fastb.overflow);
    const s3::Outcome orac = s3::finish_oracle(big, big);
    CHECK(orac.max_exceeds_u128);
    CHECK(orac.max_seen == collatz::u128_max);

    // A spread of large starts: whenever the u128 block kernel overflows, the
    // GMP block replay still returns a finite block count and a landing < n.
    std::mt19937_64 rng(0xBADF00Du);
    int checked = 0;
    for (int i = 0; i < static_cast<int>(collatz::test::scaled(3000, 200)); ++i) {
        const int bits = 100 + static_cast<int>(rng() % 27); // [100, 126]
        u128 n = collatz::make_u128(rng(), rng());
        n &= (static_cast<u128>(1) << bits) - 1;
        n |= static_cast<u128>(1) << (bits - 1);
        if (n < 3) {
            continue;
        }
        const s3::BlockOutcome fast = s3::finish_block16(n, n, 0);
        const s3::BlockOutcome ora = s3::finish_block16_oracle(n, n);
        const collatz::ref::BlockResult refb = collatz::ref::finish_blocks(n, n);
        CHECK(ora.blocks == refb.blocks);
        CHECK(ora.v_end == refb.v_end);
        CHECK(ora.v_end < n); // landing strictly below n
        if (fast.overflow) {
            ++checked;
        }
        CAPTURE(collatz::to_string(n));
    }
    CHECK(checked > 0); // the large-start spread did hit the overflow path
}
