// The pure sizing helpers of the --gpu host pipeline (src/gpu/host_pipeline.hpp).
// These tests need no OpenCL device.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "host_pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <vector>

using collatz::gpu::batch_survivor_cap;
using collatz::gpu::cand_alloc_cap;
using collatz::gpu::cand_upper_bound;
using collatz::gpu::device_headroom;
using collatz::gpu::flag_prefix_len;
using collatz::gpu::flag_rest_len;
using collatz::gpu::packed_id_survivors;
using collatz::gpu::plan_cand_cap;

TEST_CASE("candidate-id cap fits CL_DEVICE_MAX_MEM_ALLOC_SIZE") {
    constexpr std::size_t kHard = std::size_t{1} << 29;
    // RTX 3070: max_alloc 2,147,352,576 B < 2^29 * 4 = 2,147,483,648 B.
    const std::size_t c = cand_alloc_cap(2147352576ull, 4, kHard, 256);
    CHECK(c < kHard);
    CHECK(c * 4u <= 2147352576ull);
    CHECK(c % 256u == 0u);
    CHECK(c == 536838144u); // 2147352576 / 4, already a multiple of 256
    // A device allowing >= 2 GiB keeps the hard cap.
    CHECK(cand_alloc_cap(std::uint64_t{8} << 30, 4, kHard, 256) == kHard);
    CHECK(cand_alloc_cap(std::uint64_t{1} << 31, 4, kHard, 256) == kHard);
    // Unknown max_alloc (0): the hard cap.
    CHECK(cand_alloc_cap(0, 4, kHard, 256) == kHard);
    // A 16-byte-per-id buffer: the id count is clamped to what it allows.
    const std::size_t p = cand_alloc_cap(2147352576ull, 16, kHard, 256);
    CHECK(p * 16u <= 2147352576ull);
    CHECK(p % 256u == 0u);
    // A non-multiple is rounded down to the grain, never below one grain.
    CHECK(cand_alloc_cap(4u * 1000u, 4, kHard, 256) == 768u);
    CHECK(cand_alloc_cap(4u * 100u, 4, kHard, 256) == 256u);
    std::mt19937_64 rng(0xE234CA9ULL);
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t ma = (rng() % (std::uint64_t{4} << 30)) + 1024u;
        const std::uint64_t per = (rng() % 2u) != 0u ? 4u : 16u;
        const std::size_t v = cand_alloc_cap(ma, per, kHard, 256);
        CHECK(v <= kHard);
        CHECK(v % 256u == 0u);
        CHECK(v * per <= ma);
        // Maximal: one more grain would not fit (or would exceed the hard cap).
        CHECK(((v + 256u) * per > ma || v == kHard));
    }
}

TEST_CASE("device-memory budget for the candidate-id cap") {
    constexpr std::uint64_t MiB = std::uint64_t{1} << 20;
    constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
    constexpr std::size_t k3070Cap = 536838144; // cand_alloc_cap at max_alloc 2,147,352,576 B
    CHECK(device_headroom(8589410304ull) == 8589410304ull / 16u);
    CHECK(device_headroom(GiB) == 256 * MiB); // floor
    // RTX 3070 (8 GiB) at B=31 and B=32, 2^20-survivor batches, 4 B/id: fits
    // unchanged (B=32: 4 GiB bv + 256 MiB + 256 MiB + 2,147,352,576 B of ids).
    for (const std::uint64_t bv : {2 * GiB, 4 * GiB}) {
        const auto p = plan_cand_cap(8589410304ull, bv, 1u << 20, 4, k3070Cap, 1u << 20, 256);
        CHECK(p.fits);
        CHECK_FALSE(p.lowered);
        CHECK(p.cap == k3070Cap);
        CHECK(p.need == bv + (1u << 20) * 256u + 256 * MiB + std::uint64_t{k3070Cap} * 4u);
        CHECK(p.need <= p.budget);
    }
    // B=32 at 30 B/id on 8 GiB: lowered to what is left.
    {
        const auto p = plan_cand_cap(8589410304ull, 4 * GiB, 1u << 20, 30, k3070Cap, 1u << 20, 256);
        CHECK(p.fits);
        CHECK(p.lowered);
        CHECK(p.cap % 256 == 0);
        CHECK(p.cap < k3070Cap);
        CHECK(p.need <= p.budget);
        // One more grain would not fit.
        CHECK(4 * GiB + 256 * MiB + 256 * MiB + (std::uint64_t{p.cap} + 256u) * 30u > p.budget);
    }
    // A 6 GiB card at B=32: lowered.
    {
        const auto p = plan_cand_cap(6 * GiB, 4 * GiB, 1u << 20, 4, k3070Cap, 1u << 20, 256);
        CHECK(p.fits);
        CHECK(p.lowered);
        const std::uint64_t budget = 6 * GiB - 6 * GiB / 16u;
        CHECK(p.budget == budget);
        CHECK(p.cap == (budget - 4 * GiB - 512 * MiB) / 4u / 256u * 256u);
    }
    {
        // A 4 GiB card does not fit (CPU fallback); `need` reports the floor (2^20 ids).
        const auto p = plan_cand_cap(4 * GiB, 4 * GiB, 1u << 20, 4, k3070Cap, 1u << 20, 256);
        CHECK_FALSE(p.fits);
        CHECK(p.need == 4 * GiB + 512 * MiB + (std::uint64_t{1} << 20) * 4u);
    }
    // Below the floor => not fit.
    {
        const std::uint64_t g = 256 * MiB + 4 * GiB + 512 * MiB + 1000u * 4u;
        CHECK_FALSE(plan_cand_cap(g, 4 * GiB, 1u << 20, 4, k3070Cap, 4096, 256).fits);
    }
    // Randomized: whenever it fits, need <= budget and cap is in [min, cur].
    std::mt19937_64 rng(236);
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t g = (rng() % (16 * GiB)) + 1;
        const std::uint64_t bv = rng() % (8 * GiB);
        const std::uint64_t surv = rng() % (std::uint64_t{1} << 21);
        const std::uint64_t per = 1 + rng() % 32;
        const std::size_t cur = static_cast<std::size_t>(256 + rng() % (std::uint64_t{1} << 29));
        const std::size_t mn =
            static_cast<std::size_t>(std::min<std::uint64_t>(cur, 1 + rng() % (1u << 22)));
        const auto p = plan_cand_cap(g, bv, surv, per, cur, mn, 256);
        if (p.fits) {
            CHECK(p.need <= p.budget);
            CHECK(p.cap <= cur);
            CHECK(p.cap >= std::min(mn, cur));
            CHECK(p.lowered == (p.cap < cur));
        }
    }
}

TEST_CASE("the batch cap keeps every packed candidate id within 32 bits") {
    constexpr std::size_t kFlush = std::size_t{1} << 20; // the default flush cap
    // A <= 12 keeps the default cap, so the shipped A = 12 batches are unchanged.
    for (unsigned A = 5; A <= 12; ++A) {
        CHECK(batch_survivor_cap(A, kFlush) == kFlush);
    }
    CHECK(batch_survivor_cap(13, kFlush) == std::size_t{1} << 19);
    CHECK(batch_survivor_cap(14, kFlush) == std::size_t{1} << 18);
    // A smaller COLLATZ_GPU_FLUSH_CAP stays; a larger one is capped too.
    CHECK(batch_survivor_cap(13, 512) == 512u);
    CHECK(batch_survivor_cap(12, std::size_t{1} << 22) == std::size_t{1} << 20);
    for (unsigned A = 5; A <= 14; ++A) {
        INFO("A " << A);
        for (const std::size_t flush : {std::size_t{1}, kFlush, std::size_t{1} << 24}) {
            const std::size_t cap = batch_survivor_cap(A, flush);
            CHECK(cap <= flush);
            // The largest id of a full batch fits 32 bits and decodes to its
            // survivor and extension.
            const std::uint64_t a_max = (std::uint64_t{1} << A) - 1u;
            const std::uint64_t id = (static_cast<std::uint64_t>(cap - 1) << A) | a_max;
            REQUIRE(id <= 0xFFFFFFFFull);
            const auto id32 = static_cast<std::uint32_t>(id);
            CHECK((id32 >> A) == cap - 1);
            CHECK((id32 & a_max) == a_max);
        }
        // One survivor past the limit would need a 33rd bit.
        CHECK((static_cast<std::uint64_t>(packed_id_survivors(A)) << A) == std::uint64_t{1} << 32);
    }
}

TEST_CASE("flag-list prefix and remainder sizing") {
    CHECK(flag_prefix_len(1024, 65536) == 1024u);
    CHECK(flag_prefix_len(1024, 100) == 100u); // never past the buffer
    CHECK(flag_rest_len(1024, 0) == 0u);
    CHECK(flag_rest_len(1024, 7) == 0u);
    CHECK(flag_rest_len(1024, 1024) == 0u);
    CHECK(flag_rest_len(1024, 1025) == 1u);
    CHECK(flag_rest_len(0, 5) == 5u); // no prefix: everything is a device read
    // prefix + rest always reassemble exactly flag_count entries.
    for (std::size_t pref = 0; pref < 40; ++pref) {
        for (std::size_t fc = 0; fc < 80; ++fc) {
            const std::size_t used = fc < pref ? fc : pref;
            CHECK(used + flag_rest_len(used, fc) == fc);
        }
    }
}

TEST_CASE("producer cupper equals the cpi_submit loop") {
    std::mt19937_64 rng(0xC0FFEEULL);
    for (int t = 0; t < 2000; ++t) {
        const std::uint64_t two_a = std::uint64_t{1} << (rng() % 13u);
        std::vector<std::uint32_t> astar(rng() % 3000u);
        for (auto& a : astar) {
            a = static_cast<std::uint32_t>(rng() % (2u * two_a + 1u));
        }
        std::uint64_t ref = 0;
        for (const std::uint32_t a : astar) { // reference: the cpi_submit loop
            const std::uint64_t ai = a;
            ref += ai < two_a ? ai : two_a;
        }
        CHECK(cand_upper_bound(astar.begin(), astar.end(), two_a) == ref);
    }
}
