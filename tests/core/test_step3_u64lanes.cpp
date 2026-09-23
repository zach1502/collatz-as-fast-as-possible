// Step 3 u64 tail-batch columns: finish_block16_n_u64 against finish_block16_n
// on the zero-extended u128 arrays.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <string>
#include <vector>

using collatz::u128;
namespace s3 = collatz::step3;

// u64 tail-batch columns: finish_block16_n_u64 on u64 arrays equals
// finish_block16_n on the zero-extended u128 arrays in every outcome the caller
// may read, the queue contents and order, the epoch stamps, scan_needed, folded,
// fw and fp, over random batches, thresholds (including hi <= 3, the cold
// zero-extend fallback) and lane counts (1 = slot driver).

namespace {

std::string s(u128 v) {
    return collatz::to_string(v);
}

} // namespace

TEST_CASE("u64lanes: finish_block16_n_u64 == finish_block16_n on zero-extended arrays") {
    std::mt19937_64 rng(186);
    std::size_t queued = 0;
    for (unsigned trial = 0; trial < 300; ++trial) {
        const std::size_t count = 1u + static_cast<std::size_t>(rng() % s3::DeferQueue::kBatchMax);
        // tb < 12 gives hi <= 3 (cold fallback); small ones defer mid-walk.
        const unsigned tb = 2u + static_cast<unsigned>(rng() % 80u);
        const u128 thr = (static_cast<u128>(1) << tb) - 1u;
        std::vector<std::uint64_t> n64(count);
        std::vector<std::uint64_t> m64(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t n = 3u + 2u * (rng() % 2000000u);
            n64[i] = n;
            m64[i] = n + 2u * (rng() % 64u);
        }
        const std::size_t nsk = rng() % 4u;
        for (std::size_t k = 0; k < nsk; ++k) {
            const std::size_t j = rng() % count;
            if ((rng() & 1u) != 0) {
                n64[j] = 1u + rng() % 2u; // n <= 2 base case
                m64[j] = n64[j];
            } else {
                m64[j] = (std::uint64_t{1} << 55) + 1u; // above the 2^54 bar (queued)
            }
        }
        const std::vector<u128> n128(n64.begin(), n64.end());
        const std::vector<u128> m128(m64.begin(), m64.end());
        for (const std::size_t lanes : {std::size_t{1}, std::size_t{2}, std::size_t{16}}) {
            s3::DeferQueue qa;
            s3::DeferQueue qb;
            std::vector<s3::BlockOutcome> oa(count);
            std::vector<s3::BlockOutcome> ob(count);
            s3::finish_block16_n(n128.data(), m128.data(), count, lanes, thr, oa.data(), &qa);
            s3::finish_block16_n_u64(n64.data(), m64.data(), count, lanes, thr, ob.data(), &qb);
            REQUIRE(qa.epoch == qb.epoch);
            REQUIRE(qa.folded == qb.folded);
            REQUIRE(qa.scan_needed == qb.scan_needed);
            REQUIRE(qa.fw == qb.fw);
            REQUIRE(s(qa.fp) == s(qb.fp));
            REQUIRE(qa.size() == qb.size());
            for (std::size_t k = 0; k < qa.size(); ++k) {
                REQUIRE(s(qa.n[k]) == s(qb.n[k]));
                REQUIRE(s(qa.m[k]) == s(qb.m[k]));
                REQUIRE(s(qa.v[k]) == s(qb.v[k]));
                REQUIRE(qa.blk[k] == qb.blk[k]);
            }
            for (std::size_t i = 0; i < count; ++i) {
                REQUIRE(qa.deferred[i] == qb.deferred[i]);
                queued += qa.deferred[i] == qa.epoch ? 1u : 0u;
                if (qa.folded) {
                    continue; // outs not written on a folded batch
                }
                CAPTURE(i);
                REQUIRE(s(oa[i].max_seen) == s(ob[i].max_seen));
                REQUIRE(oa[i].overflow == ob[i].overflow);
                REQUIRE(oa[i].blocks == ob[i].blocks);
                REQUIRE(s(oa[i].v_end) == s(ob[i].v_end));
            }
        }
    }
    CHECK(queued > 0);
}
