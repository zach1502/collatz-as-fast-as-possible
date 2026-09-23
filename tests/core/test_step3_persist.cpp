// Step 3 persistent-lane spin of finish_block16_n against finish_block16: batch
// sizes around the lane count, a lane in the slow path, load-time gate misses
// and random batches.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <vector>

using collatz::u128;
namespace s3 = collatz::step3;

// Persistent-lane Step-3 spin (persist_fastlane, W = 24 lanes). The lane
// schedule must not change any outcome, so every per-candidate BlockOutcome
// (blocks, v_end, max_seen, overflow) must equal the single-candidate reference
// finish_block16. The batch sizes straddle the lane count W and the 8-lane group
// size, so a block-clock off-by-one in the exit service (or a lane left unparked
// at the end of a batch) shows up as a blocks mismatch. It only uses the public
// API.

namespace {

void check_batch(const std::vector<u128>& ns,
                 const std::vector<u128>& ms,
                 std::size_t count,
                 std::size_t lanes,
                 u128 thr) {
    std::vector<s3::BlockOutcome> got(count + 1);
    s3::finish_block16_n(ns.data(), ms.data(), count, lanes, thr, got.data());
    for (std::size_t i = 0; i < count; ++i) {
        const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], thr);
        CAPTURE(count);
        CAPTURE(i);
        CAPTURE(collatz::to_string(thr));
        CAPTURE(collatz::to_string(ns[i]));
        CAPTURE(collatz::to_string(ms[i]));
        REQUIRE(got[i].blocks == ref.blocks);
        REQUIRE(got[i].v_end == ref.v_end);
        REQUIRE(got[i].max_seen == ref.max_seen);
        REQUIRE(got[i].overflow == ref.overflow);
    }
}

} // namespace

TEST_CASE("persistent spin == finish_block16 at batch sizes around W and 8") {
    std::mt19937_64 rng(0x5EE3u);
    std::vector<u128> ns;
    std::vector<u128> ms;
    // Odd starts of mixed size; some m > n (a mid-trajectory start).
    for (int k = 0; k < 400; ++k) {
        const unsigned w = 4u + static_cast<unsigned>(rng() % 36u);
        const u128 n = static_cast<u128>((rng() & ((1ULL << w) - 1)) | 3u);
        ns.push_back(n);
        ms.push_back(rng() % 3u == 0 ? n + static_cast<u128>(rng() & ((1ULL << (w - 1)) - 1)) : n);
    }
    // Base cases and long-tail classics mixed in near the front, so small
    // batches see skip_candidate interleaved with live lanes.
    const std::array<std::uint64_t, 6> front = {1, 2, 27, 703, 3, 77031};
    for (std::size_t k = 0; k < front.size(); ++k) {
        ns[2 * k + 1] = front[k];
        ms[2 * k + 1] = front[k];
    }
    // Thresholds: 2^90 (hi = 2^54, pure fast lane) and small ones that put
    // bit_floor(hi) inside the trajectories, driving lanes into the v >= hi
    // slow path (deferral) and, at 5<<10, hi = 5 -> 4.
    const std::array<u128, 4> thresholds = {
        static_cast<u128>(1) << 90u,
        static_cast<u128>(1) << 30u,
        (static_cast<u128>(3) << 20u) + 7u,
        static_cast<u128>(5) << 10u,
    };
    for (const std::size_t count :
         {std::size_t{1},  std::size_t{2},  std::size_t{7},  std::size_t{8},   std::size_t{9},
          std::size_t{13}, std::size_t{15}, std::size_t{16}, std::size_t{17},  std::size_t{23},
          std::size_t{24}, std::size_t{25}, std::size_t{31}, std::size_t{32},  std::size_t{33},
          std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{131}, ns.size()}) {
        for (const u128 thr : thresholds) {
            check_batch(ns, ms, count, 16, thr);
        }
    }
}

TEST_CASE("persistent spin: one lane in the v >= bit_floor(hi) slow path, the rest descend") {
    // hi = thr >> 10 = 2^20 + 2^19, bit_floor(hi) = 2^20. 27 climbs to 9232 only,
    // 77031 peaks at 21933016 > 2^20, so it defers mid-batch while short lanes
    // around it keep retiring and refilling.
    const u128 thr = (static_cast<u128>(3) << 29u);
    std::vector<u128> ns;
    for (std::uint64_t n = 3; ns.size() < 70; n += 4) {
        ns.push_back(ns.size() == 5 ? 77031u : n);
    }
    const std::vector<u128> ms = ns;
    for (const std::size_t count : {std::size_t{6}, std::size_t{9}, std::size_t{17}, ns.size()}) {
        check_batch(ns, ms, count, 16, thr);
    }
}

TEST_CASE("persistent spin: gate misses at batch start/end, all-miss and all-hit batches") {
    // Gate misses (not admitted to a lane at load): the n <= 2 base case, a
    // start m >= bit_floor(hi) (deferred with blocks = 0), and n >= 2^64,
    // placed first, last, alone and in runs.
    const u128 big = (static_cast<u128>(1) << 70u) + 27u; // hi64 != 0
    const u128 thr = static_cast<u128>(1) << 40u;         // hi = 2^30
    const std::uint64_t over = (1ULL << 30u) + 3u;        // m >= hi at load
    auto odd = [](std::size_t k) { return 4u * static_cast<u128>(k) + 3u; };
    auto run = [&](const std::vector<u128>& ns) {
        for (const std::size_t count :
             {std::size_t{1}, std::size_t{8}, std::size_t{24}, std::size_t{25}, ns.size()}) {
            if (count <= ns.size()) {
                check_batch(ns, ns, count, 16, thr);
            }
        }
    };
    std::vector<u128> ns;
    ns.reserve(300);
    for (std::size_t k = 0; k < 300; ++k) {
        ns.push_back(odd(k));
    }
    run(ns); // all hits
    for (const u128 miss :
         {static_cast<u128>(1), static_cast<u128>(2), static_cast<u128>(over), big}) {
        std::vector<u128> all(300, miss);
        run(all); // all misses (spans two Deferred::kCap chunks)
        std::vector<u128> ends = ns;
        for (const std::size_t k : {std::size_t{0},
                                    std::size_t{1},
                                    std::size_t{23},
                                    std::size_t{24},
                                    std::size_t{255},
                                    std::size_t{256},
                                    std::size_t{299}}) {
            ends[k] = miss; // first/last of each chunk and around W
        }
        run(ends);
        std::vector<u128> alt = ns;
        for (std::size_t k = 0; k < alt.size(); k += 2) {
            alt[k] = miss;
        }
        run(alt);
    }
}

// Persistent u64 fast-lane spin (step3_persist.hpp, with its 16-B {3^f, c} block
// table). The spin layout must not change any outcome: the per-candidate
// outcomes of finish_block16_n (lanes >= 2, no queue, so the persistent spin
// runs and writes outs) equal the scalar finish_block16 reference, over random
// batches and thresholds that make lanes descend, climb past the bar and defer,
// plus n <= 2, u128 and above-bar load skips.

TEST_CASE("persistent spin: finish_block16_n outs == finish_block16 per candidate") {
    std::mt19937_64 rng(186);
    for (unsigned trial = 0; trial < 200; ++trial) {
        const std::size_t count = 1u + static_cast<std::size_t>(rng() % 1500u);
        const unsigned tb = 12u + static_cast<unsigned>(rng() % 76u);
        const u128 thr = (static_cast<u128>(1) << tb) - 1u;
        std::vector<u128> ns(count);
        std::vector<u128> ms(count);
        for (std::size_t i = 0; i < count; ++i) {
            // Mix of small and ~2^40 starts so lanes retire at different rounds.
            const std::uint64_t base = (rng() % 2u) != 0u ? (std::uint64_t{1} << 40) : 0u;
            const std::uint64_t n = base + 3u + 2u * (rng() % 4000000u);
            ns[i] = n;
            ms[i] = n + 2u * (rng() % 4096u);
        }
        const std::size_t nsk = rng() % 5u;
        for (std::size_t s = 0; s < nsk; ++s) {
            const std::size_t j = rng() % count;
            switch (rng() % 3u) {
            case 0:
                ns[j] = 1u + rng() % 2u; // n <= 2 base case
                ms[j] = ns[j];
                break;
            case 1:
                ms[j] = (static_cast<u128>(1) << 64) + 7u; // u128 start
                break;
            default:
                ms[j] = (static_cast<u128>(1) << 55) + 1u; // above the 2^54 bar
                break;
            }
        }
        for (const std::size_t lanes : {std::size_t{2}, std::size_t{16}}) {
            std::vector<s3::BlockOutcome> outs(count);
            s3::finish_block16_n(ns.data(), ms.data(), count, lanes, thr, outs.data());
            for (std::size_t i = 0; i < count; ++i) {
                const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], thr);
                CAPTURE(collatz::to_string(ns[i]));
                CAPTURE(tb);
                REQUIRE(outs[i].blocks == ref.blocks);
                REQUIRE(collatz::to_string(outs[i].v_end) == collatz::to_string(ref.v_end));
                REQUIRE(collatz::to_string(outs[i].max_seen) == collatz::to_string(ref.max_seen));
                REQUIRE(outs[i].overflow == ref.overflow);
            }
        }
    }
}
