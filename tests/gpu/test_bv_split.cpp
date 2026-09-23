// plan_bv_split (collatz/gpu/bv_split.hpp) spreads the Step-2 bitvector levels
// over up to 4 device buffers when they do not fit one allocation. These tests
// need no OpenCL device.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "collatz/gpu/bv_split.hpp"

#include <cstdint>
#include <doctest/doctest.h>

using collatz::gpu::plan_bv_split;

TEST_CASE("plan_bv_split: single buffer reproduces the flat upload") {
    // Large cap: 8 levels fit one buffer (lpb=8). buf_words_log2 = 3 + (B-5).
    const auto p = plan_bv_split(/*B=*/28, /*nbv=*/8, /*max_alloc=*/std::uint64_t{4} << 30);
    CHECK(p.ok);
    CHECK(p.num_buffers == 1);
    CHECK(p.levels_per_buffer == 8);
    CHECK(p.buf_words_log2 == 3u + (28u - 5u)); // every word maps to buffer 0.
}

TEST_CASE("plan_bv_split: 2 and 4 buffers at N=40,B=28 thresholds") {
    // Per level = 2^28/8 = 32 MiB. lpb=8 needs 256 MiB, lpb=4 128 MiB, lpb=2 64 MiB.
    const auto two = plan_bv_split(28, 8, std::uint64_t{140} << 20); // 140 MiB.
    CHECK(two.ok);
    CHECK(two.num_buffers == 2);
    CHECK(two.levels_per_buffer == 4);
    CHECK(two.buf_words_log2 == 2u + (28u - 5u));

    const auto four = plan_bv_split(28, 8, std::uint64_t{70} << 20); // 70 MiB.
    CHECK(four.ok);
    CHECK(four.num_buffers == 4);
    CHECK(four.levels_per_buffer == 2);
    CHECK(four.buf_words_log2 == 1u + (28u - 5u));
}

TEST_CASE("plan_bv_split: RTX 3070 N=44 (B=31) splits 2 GB into 2 buffers") {
    // Per level 2^31/8 = 256 MiB; 8 levels = 2 GiB > the ~2047 MiB cap. lpb=4 = 1 GiB fits.
    const std::uint64_t cap = 2147352576ull; // CL_DEVICE_MAX_MEM_ALLOC_SIZE on the 3070.
    const auto p = plan_bv_split(31, 8, cap);
    CHECK(p.ok);
    CHECK(p.num_buffers == 2);
    CHECK(p.levels_per_buffer == 4);
}

TEST_CASE("plan_bv_split: RTX 3070 B=32 splits 4 GiB into 4 buffers") {
    // Per level 2^32/8 = 512 MiB; lpb=4 = 2 GiB > the ~2047 MiB cap, so lpb=2:
    // 4 buffers of 1 GiB, bv_lg = 1 + 27 (word offsets < 2^30 fit a uint).
    const std::uint64_t cap = 2147352576ull;
    const auto p = plan_bv_split(32, 8, cap);
    CHECK(p.ok);
    CHECK(p.num_buffers == 4);
    CHECK(p.levels_per_buffer == 2);
    CHECK(p.buf_words_log2 == 1u + (32u - 5u));
    // A device whose cap is >= 4 GiB keeps one buffer.
    const auto one = plan_bv_split(32, 8, std::uint64_t{4} << 30);
    CHECK(one.ok);
    CHECK(one.num_buffers == 1);
}

TEST_CASE("plan_bv_split: fallback when one level does not fit or > 4 buffers") {
    // One level (32 MiB at B=28) larger than the cap => not ok (CPU fallback).
    CHECK_FALSE(plan_bv_split(28, 8, std::uint64_t{16} << 20).ok);
    // lpb would be 1 with nbv=8 => 8 buffers > 4 => not ok. Per level 32 MiB;
    // a cap in [32 MiB, 64 MiB) forces lpb=1.
    CHECK_FALSE(plan_bv_split(28, 8, std::uint64_t{48} << 20).ok);
    // But nbv=4 at lpb=1 is exactly 4 buffers => ok.
    const auto p = plan_bv_split(28, 4, std::uint64_t{48} << 20);
    CHECK(p.ok);
    CHECK(p.num_buffers == 4);
    CHECK(p.levels_per_buffer == 1);
}
