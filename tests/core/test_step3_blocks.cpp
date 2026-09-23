// Step 3 block finisher: finish_block16 and finish_block16_oracle against a GMP
// replay of the block recurrence, and the trivial-cycle base case.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"
#include "step1_gmp.hpp"
#include "step3_gmp.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>

using collatz::u128;
using collatz::test::mpz_to_u128;
using collatz::test::step_mpz;
namespace s3 = collatz::step3;

namespace {

// Independent GMP replay of the 16-step block recurrence with a hard cap. Mirrors
// the finisher's contract: count 16-step blocks until the trajectory first lands
// strictly below n at a block boundary. Returns false (no termination) if the cap
// is hit -- this is what a spinning finisher would trigger. On success, `final_v`
// is the boundary value that dropped below n (so final_v < n is guaranteed).
constexpr std::uint64_t kBlockCap = 1'000'000;
bool replay_blocks(u128 n, u128 m, std::uint64_t& blocks, mpz_class& final_v) {
    const mpz_class start = collatz::oracle::to_mpz(n);
    mpz_class v = collatz::oracle::to_mpz(m);
    std::uint64_t count = 0;
    while (v >= start) {
        for (unsigned j = 0; j < 16; ++j) {
            step_mpz(v);
        }
        if (++count > kBlockCap) {
            blocks = count;
            final_v = v;
            return false;
        }
    }
    blocks = count;
    final_v = v;
    return true;
}

} // namespace

// n == 2 is on the trivial cycle {1,2} and is a fixed point of the 16-step block
// map (T^16(2) == 2), so the block loop `while (v >= n)` never sees the
// mid-block descent 2->1. Without the base case the loop never ends, which hangs
// the preflight and so every table16 verify run; it must return 0 blocks.
TEST_CASE("finish_block16 terminates on the trivial-cycle base case n<=2") {
    for (u128 n : {static_cast<u128>(1), static_cast<u128>(2)}) {
        const s3::BlockOutcome bo = s3::finish_block16(n, n, 0);
        CHECK(bo.blocks == 0);
        CHECK_FALSE(bo.overflow);
        const s3::BlockOutcome oo = s3::finish_block16_oracle(n, n);
        CHECK(oo.blocks == 0);
    }
}

// finish_block16 must terminate and agree, block-for-block, with an independent
// GMP replay of the same recurrence and with finish_block16_oracle, on random
// (n, m = T^k(n) >= n) with m < 2^40. The replay's cap turns any spin into a
// visible failure rather than a hung test.
TEST_CASE("finish_block16 terminates and matches the GMP block replay") {
    std::mt19937_64 rng(0xB10C16u);
    const int kSamples = static_cast<int>(collatz::test::scaled(2000, 200));
    int compared = 0;

    for (int s = 0; s < kSamples && compared < kSamples; ++s) {
        // n in [3, 2^40): >= 3 skips the trivial base (covered above); < 2^40 with
        // m < 2^40 keeps the whole excursion far under u128 (no overflow bail).
        const u128 n = static_cast<u128>((rng() & ((1ULL << 40) - 1)) | 3u);
        const unsigned k = static_cast<unsigned>(rng() % 33); // k in [0, 32]

        const mpz_class m_mpz = collatz::test::iterate_T(collatz::oracle::to_mpz(n), k);
        if (m_mpz < collatz::oracle::to_mpz(n) || mpz_sizeinbase(m_mpz.get_mpz_t(), 2) > 40) {
            continue;
        }
        const u128 m = mpz_to_u128(m_mpz);
        ++compared;

        std::uint64_t replay_blocks_count = 0;
        mpz_class final_v;
        const bool terminated = replay_blocks(n, m, replay_blocks_count, final_v);

        CAPTURE(collatz::to_string(n));
        CAPTURE(collatz::to_string(m));
        CAPTURE(k);
        REQUIRE(terminated);                         // spin => cap hit => fail (never hang)
        CHECK(final_v < collatz::oracle::to_mpz(n)); // first boundary below n

        const s3::BlockOutcome bo = s3::finish_block16(n, m, 0);
        REQUIRE_FALSE(bo.overflow);
        CHECK(bo.blocks == replay_blocks_count);

        const s3::BlockOutcome oo = s3::finish_block16_oracle(n, m);
        CHECK(oo.blocks == replay_blocks_count);
    }
    CHECK(compared > 0);
}
