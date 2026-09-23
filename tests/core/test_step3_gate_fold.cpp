// Step 3 DeferQueue flags that flush_tail_batch reads: scan_needed (the gate
// proof) and folded with fw and fp (the inline fold).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/digest_term.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <vector>

using collatz::u128;
namespace s3 = collatz::step3;

// flush_tail_batch elides its record-gate scan when the table16 driver proves
// it empty (DeferQueue::scan_needed == false), unswitches the pass-2 fold when
// no candidate was queued this batch, and runs the scan branch-free when it
// does run. All three must give the result of the full scan and fold.
//
// Driver-level soundness: whenever the driver reports scan_needed == false, the
// scan it elides (any overflow, or any max_seen >= R = thr + 1) must be false,
// over random batches, thresholds and lane counts, including load skips (n <= 2,
// u128 m, m >= hp) and mid-walk deferrals.

namespace {

// The caller's record-gate scan over a batch: any overflow or max_seen >= R =
// thr + 1. A folded batch leaves outs unwritten; its outcomes are max_seen == m
// with no overflow, so the scan reads m, as flush_tail_batch does.
bool scan_fires(const s3::DeferQueue& dq,
                const std::vector<s3::BlockOutcome>& outs,
                const std::vector<u128>& ms,
                std::size_t count,
                u128 thr) {
    const u128 r = thr + 1; // the flush passes thr = R - 1 (R > 0 on the fast path)
    for (std::size_t i = 0; i < count; ++i) {
        const bool fires = dq.folded ? ms[i] >= r : (outs[i].overflow || outs[i].max_seen >= r);
        if (fires) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("gate proof: scan_needed == false implies the record-gate scan is empty") {
    std::mt19937_64 rng(179);
    const std::size_t count = 256;
    std::size_t proved = 0;
    std::size_t flagged = 0;
    for (unsigned trial = 0; trial < 400; ++trial) {
        // Thresholds from tiny (fast lane off: hi <= 3) to beyond the 2^54 bar.
        const unsigned tb = 8u + static_cast<unsigned>(rng() % 80u);
        const u128 thr = (static_cast<u128>(1) << tb) - 1u;
        std::vector<u128> ns(count);
        std::vector<u128> ms(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t n = 3u + 2u * (rng() % 200000u);
            ns[i] = n;
            ms[i] = n + 2u * (rng() % 64u); // m >= n; small, admitted when thr is large
        }
        // A third of the batches carry one load-time skip of a random kind.
        if (trial % 3 == 0) {
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
        for (const std::size_t lanes : {std::size_t{1}, std::size_t{2}, std::size_t{16}}) {
            s3::DeferQueue dq;
            std::vector<s3::BlockOutcome> outs(count);
            s3::finish_block16_n(ns.data(), ms.data(), count, lanes, thr, outs.data(), &dq);
            if (!dq.scan_needed) {
                ++proved;
                CHECK_FALSE(scan_fires(dq, outs, ms, count, thr));
                CHECK(lanes >= 2);
            } else {
                ++flagged;
            }
        }
    }
    // Both outcomes must be exercised.
    CHECK(proved > 0);
    CHECK(flagged > 0);
}

TEST_CASE("gate proof: a load-time skip keeps the scan on") {
    const std::size_t count = 64;
    std::vector<u128> ns(count);
    std::vector<u128> ms(count);
    for (std::size_t i = 0; i < count; ++i) {
        ns[i] = 101u + 2u * i;
        ms[i] = ns[i];
    }
    const u128 thr = (static_cast<u128>(1) << 70) - 1u;
    {
        s3::DeferQueue dq;
        std::vector<s3::BlockOutcome> outs(count);
        s3::finish_block16_n(ns.data(), ms.data(), count, 16, thr, outs.data(), &dq);
        CHECK_FALSE(dq.scan_needed); // all admitted: the proof holds
    }
    // One candidate whose m is at or above the record R: it is load-deferred,
    // its gate value m trips the gate, so the flag must stay set.
    ms[17] = thr + 1u;
    {
        s3::DeferQueue dq;
        std::vector<s3::BlockOutcome> outs(count);
        s3::finish_block16_n(ns.data(), ms.data(), count, 16, thr, outs.data(), &dq);
        CHECK(dq.scan_needed);
        CHECK(scan_fires(dq, outs, ms, count, thr));
    }
}

// Fold-inline: the u64 fast-lane spin folds each descended candidate's block
// count and pow digest term into DeferQueue::fw / fp, and flush_tail_batch
// skips its pass-2 fold for the batch (DeferQueue::folded). The inline fold must
// be bit-identical to the pass-2 fold.
//
// Driver level: whenever the driver reports folded, fw / fp equal the sums of
// finish_block16's blocks and the same result_digest_term over every candidate
// not queued this batch (queued ones are folded at drain), over random batches,
// thresholds and lane counts, including n <= 2 base cases, u128 and above-bar
// load skips and mid-walk deferrals. A batch the driver did not fold carries the
// full outcomes in outs.

TEST_CASE("fold-inline: driver fw/fp == pass-2 sums over non-queued candidates") {
    std::mt19937_64 rng(184);
    std::size_t plain_batches = 0;
    for (unsigned trial = 0; trial < 300; ++trial) {
        const std::size_t count = 1u + static_cast<std::size_t>(rng() % s3::DeferQueue::kBatchMax);
        // Thresholds from tiny (fast lane off) to beyond the 2^54 bar; small
        // ones make lanes climb past hp and defer mid-walk.
        const unsigned tb = 8u + static_cast<unsigned>(rng() % 80u);
        const u128 thr = (static_cast<u128>(1) << tb) - 1u;
        std::vector<u128> ns(count);
        std::vector<u128> ms(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t n = 3u + 2u * (rng() % 2000000u);
            ns[i] = n;
            ms[i] = n + 2u * (rng() % 64u);
        }
        const std::size_t nsk = rng() % 4u;
        for (std::size_t s = 0; s < nsk; ++s) {
            const std::size_t j = rng() % count;
            switch (rng() % 3u) {
            case 0:
                ns[j] = 1u + rng() % 2u; // n <= 2 base case (folded inline)
                ms[j] = ns[j];
                break;
            case 1:
                ms[j] = (static_cast<u128>(1) << 64) + 7u; // u128 start (queued)
                break;
            default:
                ms[j] = (static_cast<u128>(1) << 55) + 1u; // above the 2^54 bar (queued)
                break;
            }
        }
        std::vector<s3::BlockOutcome> ref(count);
        for (std::size_t i = 0; i < count; ++i) {
            ref[i] = s3::finish_block16(ns[i], ms[i], thr);
        }
        for (const std::size_t lanes : {std::size_t{1}, std::size_t{2}, std::size_t{16}}) {
            s3::DeferQueue dq;
            std::vector<s3::BlockOutcome> outs(count);
            for (auto& o : outs) {
                o.blocks = 0xDEADBEEFULL; // poison: a folded batch must not need these
                o.v_end = 0xDEADBEEFULL;
            }
            s3::finish_block16_n(ns.data(), ms.data(), count, lanes, thr, outs.data(), &dq);
            std::uint64_t sw = 0;
            u128 sp = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (dq.deferred[i] == dq.epoch) {
                    continue; // queued this batch: folded at drain
                }
                sw += ref[i].blocks;
                sp += collatz::digest::result_digest_term(ns[i], ref[i].blocks, ref[i].v_end);
                CAPTURE(collatz::to_string(ns[i]));
                if (!dq.folded) {
                    REQUIRE(outs[i].blocks == ref[i].blocks);
                    REQUIRE(outs[i].v_end == ref[i].v_end);
                } else {
                    // A folded, non-queued outcome has max_seen == m and never
                    // overflows, so the caller may read b.m instead of outs[i].
                    REQUIRE(collatz::to_string(ref[i].max_seen) == collatz::to_string(ms[i]));
                    REQUIRE(!ref[i].overflow);
                }
            }
            if (dq.folded) {
                CHECK(lanes >= 2);
                CHECK(dq.fw == sw);
                // u128 has no doctest stringifier: compare decimal strings.
                CHECK(collatz::to_string(dq.fp) == collatz::to_string(sp));
            } else {
                ++plain_batches;
            }
        }
    }
    CHECK(plain_batches > 0);
}
