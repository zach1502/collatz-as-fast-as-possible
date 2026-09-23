// The raw-sum fold of u128_limbs.h, checked on the host. The per-survivor
// kernel folds cov = mod61(cov + mod61(term_i)); a tail_persistent lane sums
// the raw terms in a u128 (lo + hi with carry) and reduces mod P = 2^61-1 once,
// with u128q_mod_mersenne61_u128. Reduction mod P is a ring homomorphism, so
// Sum_i (term_i mod P) == (Sum_i term_i) mod P and both give the same per-lane
// residue < P, the value that enters the cross-lane tree reduction. The u128
// reducer is checked against collatz::u128 (an __int128 alias that silences
// -Wpedantic), so `ctest -L unit` covers the identity without a GPU.

#include "collatz/gpu/u128_limbs.h"
#include "collatz/u128.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <utility>
#include <vector>

using collatz::u128;

namespace {

constexpr std::uint64_t kP = 2305843009213693951ULL; // 2^61 - 1

// Reference: (lo + hi*2^64) mod (2^61 - 1) via 128-bit division.
[[nodiscard]] std::uint64_t ref_mod61_u128(std::uint64_t lo, std::uint64_t hi) {
    const collatz::u128 x = (static_cast<collatz::u128>(hi) << 64) | lo;
    return static_cast<std::uint64_t>(x % kP);
}

} // namespace

TEST_CASE("u128q_mod_mersenne61_u128 matches a 128-bit reference") {
    const std::pair<std::uint64_t, std::uint64_t> edges[] = {
        {0, 0},
        {kP - 1, 0},
        {kP, 0},
        {kP + 1, 0},
        {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
        {0, 1},
        {0, 0xFFFFFFFFFFFFFFFFULL},
        {1ULL << 61, 0},
        {0, 1ULL << 57},
        {0, 1ULL << 58},
    };
    for (const auto& [lo, hi] : edges) {
        CHECK(u128q_mod_mersenne61_u128(lo, hi) == ref_mod61_u128(lo, hi));
    }

    std::mt19937_64 rng(0xE230C0DEULL);
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t lo = rng();
        const std::uint64_t hi = rng();
        CHECK(u128q_mod_mersenne61_u128(lo, hi) == ref_mod61_u128(lo, hi));
        CHECK(u128q_mod_mersenne61_u128(lo, hi) < kP);
    }
}

TEST_CASE("raw-sum-then-reduce equals the per-term mod61 fold") {
    std::mt19937_64 rng(0x5151A11ULL);
    for (int trial = 0; trial < 5000; ++trial) {
        const int n = static_cast<int>(rng() % 400);
        std::uint64_t running = 0; // per-term fold: Sum_i mod61(term_i) mod P
        std::uint64_t acc_lo = 0;
        std::uint64_t acc_hi = 0;
        for (int i = 0; i < n; ++i) {
            const std::uint64_t term = rng();
            running = u128q_mod_mersenne61(running + u128q_mod_mersenne61(term));
            const std::uint64_t before = acc_lo;
            acc_lo += term;
            if (acc_lo < before) {
                ++acc_hi;
            }
        }
        CHECK(u128q_mod_mersenne61_u128(acc_lo, acc_hi) == running);
    }
}

TEST_CASE("the raw-sum identity holds for the actual coverage / digest terms") {
    std::mt19937_64 rng(0xD16E57ULL);
    std::uint64_t cov_run = 0;
    std::uint64_t dig_run = 0;
    std::uint64_t cov_lo = 0, cov_hi = 0, dig_lo = 0, dig_hi = 0;
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t n = rng() >> 24; // n < 2^40
        const std::uint64_t blocks = rng() % 64;
        const u128q vend = u128q_from_halves(rng() >> 20, 0);
        const std::uint64_t ct = u128q_mix64(u128q_cov_key_n64(n));
        const std::uint64_t dt = u128q_result_digest_term_n64(n, blocks, vend);
        cov_run = u128q_mod_mersenne61(cov_run + u128q_mod_mersenne61(ct));
        dig_run = u128q_mod_mersenne61(dig_run + u128q_mod_mersenne61(dt));
        std::uint64_t b = cov_lo;
        cov_lo += ct;
        if (cov_lo < b) {
            ++cov_hi;
        }
        b = dig_lo;
        dig_lo += dt;
        if (dig_lo < b) {
            ++dig_hi;
        }
    }
    CHECK(u128q_mod_mersenne61_u128(cov_lo, cov_hi) == cov_run);
    CHECK(u128q_mod_mersenne61_u128(dig_lo, dig_hi) == dig_run);
}

// Strength of the pow result digest ("v5" in the test names) against the plain
// wrapping sum of the 16-step block counts over all candidates ("v3", reported
// as blocks_total). The plain sum has two weaknesses the digest does not have:
//   (1) it ignores v_end, so a wrong landing value is invisible;
//   (2) two compensating block errors (+1 on one candidate, -1 on another)
//       cancel, so a pair of wrong triples leaves it unchanged.
// The digest folds mix64(...(n, blocks, v_end)...) mod 2^61-1, so any single
// wrong (n, blocks, v_end) triple changes it, and compensating pairs do too.
//
// The digest term is the shared u128q_result_digest_term of
// include/collatz/gpu/u128_limbs/u128_limbs_06_folds_knobs.h, byte-identical to
// the CPU verifier's result_digest_term (collatz/digest_term.hpp) and to the
// OpenCL kernels; the GPU-vs-CPU tests (test_integration_cpu_gpu_agreement)
// check this end to end.

namespace {

// One checked candidate summarized by the (n, blocks, v_end) triple the digest
// binds. v_end < n on a real descent, but the digest never assumes that.
struct Triple {
    u128 n;
    std::uint64_t blocks;
    u128 v_end;
};

[[nodiscard]] u128q toq(u128 x) {
    return u128q_from_halves(collatz::lo64(x), collatz::hi64(x));
}

// The pow result digest: sum mod P of the per-candidate result-digest terms.
[[nodiscard]] std::uint64_t digest(const std::vector<Triple>& set) {
    std::uint64_t acc = 0;
    for (const Triple& t : set) {
        const std::uint64_t term = u128q_result_digest_term(toq(t.n), t.blocks, toq(t.v_end));
        acc = u128q_mod_mersenne61(acc + u128q_mod_mersenne61(term));
    }
    return acc;
}

// The plain wrapping sum of block counts (blocks_total); it ignores v_end.
[[nodiscard]] std::uint64_t v3_block_sum(const std::vector<Triple>& set) {
    std::uint64_t s = 0;
    for (const Triple& t : set) {
        s += t.blocks;
    }
    return s;
}

// A deterministic synthetic candidate set resembling a real N~40 tail: n around
// 2^40, a few tens of blocks, v_end strictly below n.
[[nodiscard]] std::vector<Triple> make_set(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Triple> set;
    set.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const u128 n = static_cast<u128>((rng() & ((1ULL << 40) - 1)) | 1u) + 3u;
        const std::uint64_t blocks = (rng() % 60) + 1;
        const u128 v_end = static_cast<u128>(rng() % collatz::lo64(n)); // 0 <= v_end < n
        set.push_back({n, blocks, v_end});
    }
    return set;
}

} // namespace

TEST_CASE("pow-v5 digest is in [0, 2^61-1) and order-independent") {
    const std::vector<Triple> set = make_set(500, 1);
    const std::uint64_t d = digest(set);
    CHECK(d < kP);

    // Order independence: reversing the fold gives the identical digest (the
    // sum mod P is commutative), matching the thread/lane-independence contract.
    std::vector<Triple> rev(set.rbegin(), set.rend());
    CHECK(digest(rev) == d);
}

TEST_CASE("flipping one candidate's blocks changes the v5 digest") {
    std::vector<Triple> set = make_set(1000, 2);
    const std::uint64_t d0 = digest(set);
    set[417].blocks += 1; // a single wrong block count
    const std::uint64_t d1 = digest(set);
    CHECK(d1 != d0);
}

TEST_CASE("flipping one candidate's v_end changes the v5 digest") {
    // The block sum ignores v_end, so a wrong landing value is invisible to
    // it; the digest binds v_end, so it changes.
    std::vector<Triple> set = make_set(1000, 3);
    const std::uint64_t d0 = digest(set);
    const std::uint64_t s0 = v3_block_sum(set);
    set[813].v_end = set[813].v_end + 1; // a single wrong landing value
    CHECK(digest(set) != d0);
    CHECK(v3_block_sum(set) == s0); // the block sum cannot catch this
}

TEST_CASE("compensating +1/-1 block errors change v5 digest but not the v3 sum") {
    // Candidate A over-counts by one block and candidate B under-counts by
    // one. The block sum is unchanged (the errors cancel), so a check on it
    // alone would accept the corrupted run; the digest changes with
    // overwhelming probability.
    std::vector<Triple> set = make_set(1000, 4);
    const std::uint64_t d0 = digest(set);
    const std::uint64_t s0 = v3_block_sum(set);

    REQUIRE(set[100].blocks >= 1);
    REQUIRE(set[900].blocks >= 2);
    set[100].blocks += 1;
    set[900].blocks -= 1;

    CHECK(v3_block_sum(set) == s0); // the compensating errors cancel in the sum
    CHECK(digest(set) != d0);       // and change the digest
}

TEST_CASE("two synthetic sets differing in exactly one triple have different digests") {
    const std::vector<Triple> a = make_set(2000, 5);
    std::vector<Triple> b = a;
    b[1234].n = b[1234].n + 2; // a single different candidate identity
    CHECK(digest(a) != digest(b));
}
