// Step 3 lane driver finish_block16_n (u64 fast lane, persistent lanes, exit
// mask, a reused output buffer) against the single-candidate finish_block16,
// field by field.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "scale.hpp"
#include "step1_gmp.hpp"
#include "step3_gmp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <vector>

using collatz::u128;
using collatz::test::mpz_to_u128;
using collatz::test::step_mpz;
namespace s3 = collatz::step3;

// The SoA lane driver runs each block in u64 arithmetic while `v < 2^54` and
// promotes to the u128 path once a block climbs above that bar. This must be
// bit-identical to the pure-u128 single finisher `finish_block16` (which the driver
// does not touch) for any starting value, and to the independent GMP oracle. The test
// spans m from below 2^54 (pure u64) through ~2^62 (starts on the u128 path, then
// descends across the 2^54 boundary, exercising promotion mid-tail), across several
// lane counts and several record thresholds (a low threshold forces the contender
// path, which the fast lane must decline to and defer to the u128 path).
// Bit-identical `blocks`, `max_seen`, and `overflow` are the exactness contract for
// the fast lane.
TEST_CASE("finish_block16_n u64 fast lane == pure-u128 finish_block16 and oracle") {
    std::mt19937_64 rng(0xE39u);
    const int kSamples = static_cast<int>(collatz::test::scaled(6000, 3000));

    std::vector<u128> ns;
    std::vector<u128> ms;
    // Build a batch of (n, m = T^k(n) >= n) pairs. n up to 2^30 and a
    // long walk lets m range from tiny up past 2^54 (some path-record climbs),
    // so both the u64 lane and the u128 promotion path are exercised; cap m at
    // 2^96 to stay far from the u128 overflow bail (which is a separate contract).
    for (int s = 0; s < kSamples; ++s) {
        const u128 n = static_cast<u128>((rng() & ((1ULL << 30) - 1)) | 3u);
        const unsigned k = static_cast<unsigned>(rng() % 200);
        const mpz_class m_mpz = collatz::test::iterate_T(collatz::oracle::to_mpz(n), k);
        if (m_mpz < collatz::oracle::to_mpz(n) || mpz_sizeinbase(m_mpz.get_mpz_t(), 2) > 96) {
            continue;
        }
        ns.push_back(n);
        ms.push_back(mpz_to_u128(m_mpz));
    }
    REQUIRE(ns.size() > 50);

    // A few thresholds: 0 (exact max everywhere -> contender fires every block,
    // so the fast lane defers to u128 constantly), a mid bar, and a high bar
    // (the verifier's regime, where essentially every eligible block goes u64).
    const std::array<u128, 3> thresholds = {
        static_cast<u128>(0),
        static_cast<u128>(1) << 40u,
        static_cast<u128>(1) << 90u,
    };
    const std::array<std::size_t, 4> lane_counts = {1u, 4u, 16u, 32u};

    for (const u128 thr : thresholds) {
        // Pure-u128 reference: the single-candidate finish_block16 has no u64 lane.
        std::vector<s3::BlockOutcome> ref(ns.size());
        for (std::size_t i = 0; i < ns.size(); ++i) {
            ref[i] = s3::finish_block16(ns[i], ms[i], thr);
        }
        for (const std::size_t lanes : lane_counts) {
            std::vector<s3::BlockOutcome> got(ns.size());
            s3::finish_block16_n(ns.data(), ms.data(), ns.size(), lanes, thr, got.data());
            for (std::size_t i = 0; i < ns.size(); ++i) {
                CAPTURE(collatz::to_string(ns[i]));
                CAPTURE(collatz::to_string(ms[i]));
                CAPTURE(lanes);
                CHECK(got[i].blocks == ref[i].blocks);
                CHECK(got[i].max_seen == ref[i].max_seen);
                CHECK(got[i].overflow == ref[i].overflow);
                // Cross-check the exact-max threshold-0 run against the GMP oracle.
                if (thr == 0 && !ref[i].overflow) {
                    const s3::BlockOutcome oo = s3::finish_block16_oracle(ns[i], ms[i]);
                    CHECK(got[i].blocks == oo.blocks);
                    CHECK(got[i].max_seen == oo.max_seen);
                }
            }
        }
    }
}

// Persistent-lane refill driver: batches larger than one internal chunk, n <= 2
// base cases, m >= 2^54 and m >= thr >> 10 starts (deferred at load), and lanes
// that climb out of the u64 lane mid-tail (deferred with state) must all match
// finish_block16 in every BlockOutcome field, including v_end.
TEST_CASE("finish_block16_n persistent lanes == finish_block16 in every field") {
    std::mt19937_64 rng(0x7A11u);
    std::vector<u128> ns;
    std::vector<u128> ms;
    for (int s = 0; s < 1500; ++s) {
        const unsigned kind = static_cast<unsigned>(rng() % 16);
        if (kind == 0) {
            const u128 n = 1u + static_cast<u128>(rng() & 1u);
            ns.push_back(n);
            ms.push_back(n + static_cast<u128>(rng() & 7u));
            continue;
        }
        // kind 1: n up to 2^62 so m can start at or climb past 2^54.
        const std::uint64_t nmask = kind == 1 ? ((1ULL << 62) - 1) : ((1ULL << 40) - 1);
        const u128 n = static_cast<u128>((rng() & nmask) | 3u);
        const mpz_class n_mpz = collatz::oracle::to_mpz(n);
        mpz_class x = n_mpz;
        const unsigned k = static_cast<unsigned>(rng() % 40);
        for (unsigned j = 0; j < k; ++j) {
            mpz_class y = x;
            step_mpz(y);
            if (y < n_mpz) {
                break;
            }
            x = y;
        }
        ns.push_back(n);
        ms.push_back(mpz_to_u128(x));
    }
    REQUIRE(ns.size() > 1000);
    const std::array<u128, 4> thresholds = {
        static_cast<u128>(0),
        static_cast<u128>(1) << 50u,
        static_cast<u128>(1) << 60u,
        static_cast<u128>(1) << 90u,
    };
    for (const u128 thr : thresholds) {
        for (const std::size_t lanes : {2u, 16u}) {
            std::vector<s3::BlockOutcome> got(ns.size());
            s3::finish_block16_n(ns.data(), ms.data(), ns.size(), lanes, thr, got.data());
            for (std::size_t i = 0; i < ns.size(); ++i) {
                const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], thr);
                CAPTURE(collatz::to_string(ns[i]));
                CAPTURE(collatz::to_string(ms[i]));
                CHECK(got[i].blocks == ref.blocks);
                CHECK(got[i].v_end == ref.v_end);
                CHECK(got[i].max_seen == ref.max_seen);
                CHECK(got[i].overflow == ref.overflow);
            }
        }
    }
}

// Batched lane-exit mask: exits are detected once per round for all lanes, the
// fast-lane bar is rounded down to a power of two, and exiting lanes are
// serviced in bit order. Starts at and around the bars, counts around the lane
// width and the 256 chunk, and thresholds whose bar is not a power of two must
// all match finish_block16 field by field.
TEST_CASE("finish_block16_n exit-mask spin == finish_block16 at the edges") {
    std::mt19937_64 rng(0xE1A5u);
    std::vector<u128> ns;
    std::vector<u128> ms;
    const auto add = [&](u128 n, u128 m) {
        ns.push_back(n);
        ms.push_back(m);
    };
    const std::array<unsigned, 8> bars = {20, 31, 40, 52, 53, 54, 55, 62};
    for (const unsigned b : bars) {
        const u128 p = static_cast<u128>(1) << b;
        for (const u128 d : {static_cast<u128>(1), static_cast<u128>(2), static_cast<u128>(7)}) {
            add((p - d) | 3u, (p - d) | 3u);
            add(((p >> 1) - d) | 3u, p - d);
            add(((p >> 1) - d) | 3u, p);
            add(((p >> 1) - d) | 3u, p + d);
        }
    }
    add(1, 1);
    add(2, 2);
    add(2, 9);
    add(3, 3);
    add(3, 16);
    add(7, 7);
    add(27, 27);
    add(27, 9232);
    for (int s = 0; s < 1200; ++s) {
        const unsigned w = 3u + static_cast<unsigned>(rng() % 60u);
        const u128 n = static_cast<u128>((rng() & ((1ULL << w) - 1)) | 3u);
        const unsigned up = static_cast<unsigned>(rng() % 4u);
        add(n, up == 0 ? n : n + static_cast<u128>(rng() & ((1ULL << (w - 1)) - 1)));
    }
    const std::array<u128, 8> thresholds = {
        static_cast<u128>(0),
        static_cast<u128>(5) << 10u,
        static_cast<u128>(3) << 40u,
        (static_cast<u128>(1) << 50u) + 12345u,
        static_cast<u128>(7) << 60u,
        static_cast<u128>(1) << 64u,
        (static_cast<u128>(1) << 64u) - 1u,
        static_cast<u128>(1) << 90u,
    };
    for (const std::size_t count : {std::size_t{0},
                                    std::size_t{1},
                                    std::size_t{7},
                                    std::size_t{8},
                                    std::size_t{9},
                                    std::size_t{17},
                                    std::size_t{255},
                                    std::size_t{256},
                                    std::size_t{257},
                                    ns.size()}) {
        for (const u128 thr : thresholds) {
            for (const std::size_t lanes : {2u, 16u}) {
                std::vector<s3::BlockOutcome> got(count + 1);
                s3::finish_block16_n(ns.data(), ms.data(), count, lanes, thr, got.data());
                for (std::size_t i = 0; i < count; ++i) {
                    const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], thr);
                    CAPTURE(count);
                    CAPTURE(collatz::to_string(thr));
                    CAPTURE(collatz::to_string(ns[i]));
                    CAPTURE(collatz::to_string(ms[i]));
                    REQUIRE(got[i].blocks == ref.blocks);
                    REQUIRE(got[i].v_end == ref.v_end);
                    REQUIRE(got[i].max_seen == ref.max_seen);
                    REQUIRE(got[i].overflow == ref.overflow);
                }
            }
        }
    }
}

// flush_tail_batch reuses one TailBatch::outs buffer across batches without
// re-zeroing it, so every finish_block16_n path (persistent lanes, their
// deferred/u128 slot driver and the lanes == 1 slot driver) must write every
// outs[0, count) slot. Poison the buffer with a sentinel and reuse it, unwiped,
// across lane counts and thresholds; any slot a driver skips keeps stale data
// and fails the field-by-field comparison.
TEST_CASE("finish_block16_n writes every outcome slot of a poisoned, reused buffer") {
    std::mt19937_64 rng(0x5107u);
    std::vector<u128> ns;
    std::vector<u128> ms;
    for (int s = 0; s < 700; ++s) {
        const unsigned kind = static_cast<unsigned>(rng() % 16);
        if (kind == 0) {
            const u128 n = 1u + static_cast<u128>(rng() & 1u);
            ns.push_back(n);
            ms.push_back(n);
            continue;
        }
        // kind 1: large starts so the persistent spin defers some at load or
        // mid-tail into the u128 slot driver.
        const std::uint64_t nmask = kind == 1 ? ((1ULL << 62) - 1) : ((1ULL << 40) - 1);
        const u128 n = static_cast<u128>((rng() & nmask) | 3u);
        ns.push_back(n);
        ms.push_back(n);
    }
    s3::BlockOutcome poison;
    poison.blocks = 0xDEADBEEFULL;
    poison.v_end = static_cast<u128>(0xBADULL) << 70u;
    poison.max_seen = static_cast<u128>(0xBADULL) << 80u;
    poison.overflow = true;
    std::vector<s3::BlockOutcome> buf(ns.size(), poison);
    const std::array<u128, 3> thresholds = {
        static_cast<u128>(0), static_cast<u128>(1) << 50u, static_cast<u128>(1) << 90u};
    for (const std::size_t lanes : {1u, 2u, 16u}) {
        for (const u128 thr : thresholds) {
            s3::finish_block16_n(ns.data(), ms.data(), ns.size(), lanes, thr, buf.data());
            for (std::size_t i = 0; i < ns.size(); ++i) {
                const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], thr);
                CAPTURE(lanes);
                CAPTURE(collatz::to_string(ns[i]));
                REQUIRE(buf[i].blocks == ref.blocks);
                REQUIRE(buf[i].v_end == ref.v_end);
                REQUIRE(buf[i].max_seen == ref.max_seen);
                REQUIRE(buf[i].overflow == ref.overflow);
            }
        }
    }
}
