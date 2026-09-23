// Step 3 multi-lane 16-step block drivers: the batch driver finish_block16_n_impl
// behind finish_block16_n and finish_block16_n_u64, and drain_deferred.

#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "step3_chain.hpp"
#include "step3_internal.hpp"
#include "step3_persist.hpp"
#include "step3_persist_run.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace collatz::step3 {

namespace detail {

[[nodiscard]] const BlockTable16& block_table16() {
    static const StaticTable<std::uint32_t, kBlockEntries> table([](auto& t) {
        const std::size_t size = t.size();
        for (std::size_t r = 0; r < size; ++r) {
            u128 v = static_cast<u128>(r);
            unsigned f = 0;
            for (unsigned j = 0; j < kBlockStep; ++j) {
                if ((v & 1) != 0) {
                    ++f;
                }
                v = collatz::T(v).value; // r < 2^16 never overflows u128
            }
            t[r] = (static_cast<std::uint32_t>(v) << 5u) | static_cast<std::uint32_t>(f);
        }
    });
    return table.t;
}

} // namespace detail

using namespace detail;

/// Core of finish_block16_n: the persistent u64 spin for a whole batch when it
/// applies, else the refilling slot driver. `maxv0` / `blks` (per candidate) or
/// `blocks_init` (shared) resume candidates mid-tail, as the deferred-queue
/// drains do.
void finish_block16_n_impl(const u128* ns,
                           const u128* ms,
                           std::size_t count,
                           std::size_t lanes,
                           u128 max_threshold,
                           BlockOutcome* outs,
                           std::uint64_t blocks_init = 0,
                           const u128* maxv0 = nullptr,
                           const std::uint64_t* blks = nullptr,
                           DeferQueue* dq = nullptr) noexcept {
    const u128 fc = max_threshold >> 10;
    const std::uint64_t hi =
        hi64(fc) != 0 ? lo64(kU64FastBar) : std::min(lo64(fc), lo64(kU64FastBar));
    // The queue epoch advances once per call instead of a clear of kBatchMax
    // entries: a candidate is queued in this batch iff deferred[i] == epoch, so
    // a batch that does not take the fast lane leaves no index matching the
    // current epoch. It advances before the size check, so stale stamps never
    // match.
    if (dq != nullptr) {
        ++dq->epoch;
        // Pessimistic until the fast path below proves otherwise.
        dq->scan_needed = true;
        dq->folded = false; // fold-inline: set only on the dq fast-path return
    }
    // The cross-batch queue holds a per-batch mask of size kBatchMax.
    assert(dq == nullptr || count <= DeferQueue::kBatchMax);
    if (maxv0 == nullptr && blks == nullptr && blocks_init == 0 && lanes >= 2 && hi > 3) {
        persist_batch(ns, ms, count, hi, outs, dq, [&](const Deferred& d, std::size_t base) {
            static thread_local std::array<BlockOutcome, Deferred::kCap> dout;
            finish_block16_n_impl(
                d.n, d.v, d.count, lanes, max_threshold, dout.data(), 0, d.maxv, d.blk);
            for (std::size_t k = 0; k < d.count; ++k) {
                outs[base + d.idx[k]] = dout[k];
            }
        });
        return;
    }
    // Every output starts as the trivial-cycle base result (0 blocks,
    // max_seen = m), as in finish_block16's n <= 2 base case; active candidates
    // overwrite it when they retire.
    for (std::size_t i = 0; i < count; ++i) {
        outs[i] = BlockOutcome{};
        outs[i].max_seen = ms[i];
        outs[i].v_end = ns[i]; // base-case landing (n<=2 candidates load inactive).
    }
    if (count == 0) {
        return;
    }

    const BlockTable16& tb = block_table16();

    // Number of dependent chains in flight at once (hardware lanes), bounded by
    // the slot array and by the candidate count.
    constexpr std::size_t kMaxSlots = 32;
    std::size_t slots = lanes < 1 ? 1 : lanes;
    if (slots > kMaxSlots) {
        slots = kMaxSlots;
    }
    if (slots > count) {
        slots = count;
    }

    std::array<BlockChain, kMaxSlots> chain{};

    // Loads candidate `i` into slot `s`. A candidate with n <= 2 is the trivial
    // base case: it loads inactive (its out already holds the base result), so
    // the driver refills that slot in the next iteration. A resumed candidate starts
    // from its running max maxv0[i] and block count blks[i] (or blocks_init)
    // instead of m / 0; load runs once per candidate, not per block.
    const auto load = [&](std::size_t s, std::size_t i) noexcept {
        BlockChain& c = chain[s];
        c.v = ms[i];
        c.maxv = (maxv0 != nullptr) ? maxv0[i] : ms[i];
        c.n = ns[i];
        c.threshold = max_threshold;
        c.blocks = (blks != nullptr) ? blks[i] : blocks_init;
        c.active = ns[i] > 2;
        c.out = &outs[i];
    };

    std::size_t next = 0;
    for (std::size_t s = 0; s < slots; ++s) {
        load(s, next++);
    }

    // Refilling lane loop: every live slot takes one 16-step block, then a slot
    // whose chain retired takes the next pending candidate, so no slot idles
    // while work remains. The chains are independent, so the overlap never
    // changes a per-candidate result.
    //
    // Each iteration gathers the active slots in the u64 fast lane and drains
    // them in register groups through spin_fastlane, so an out-of-order core
    // overlaps their independent, latency-bound madd chains. Block counts, v_end
    // and max_seen are bit-identical (see spin_fastlane). After the spin every
    // gathered chain has left its fast lane, so the block_step pass that follows
    // only does the retire, promote or exact-max transition (and advances the
    // u128-path and lone fast-lane slots).
    for (;;) {
        std::array<std::size_t, kMaxSlots> elig{};
        std::size_t ne = 0;
        for (std::size_t s = 0; s < slots; ++s) {
            const BlockChain& c = chain[s];
            if (!c.active || c.v < c.n) {
                continue; // inactive or pending-retire: block_step handles it
            }
            const u128 bar = c.maxv > c.threshold ? c.maxv : c.threshold;
            if (c.v < kU64FastBar && c.v < (bar >> 10)) {
                elig[ne++] = s; // matches block_step's fast-lane gate
            }
        }
        drain_fastlane(elig, ne, chain, tb);

        // One block_step per active slot (the transition of each drained chain,
        // or one block of a u128-path or lone fast-lane slot), then the retired
        // slots are refilled.
        bool any_live = false;
        for (std::size_t s = 0; s < slots; ++s) {
            if (chain[s].active) {
                block_step(chain[s], tb);
            }
            if (!chain[s].active && next < count) {
                load(s, next++);
            }
            any_live = any_live || chain[s].active;
        }
        if (!any_live && next >= count) {
            break;
        }
    }
}

void finish_block16_n(const u128* ns,
                      const u128* ms,
                      std::size_t count,
                      std::size_t lanes,
                      u128 max_threshold,
                      BlockOutcome* outs,
                      DeferQueue* dq) noexcept {
    finish_block16_n_impl(ns, ms, count, lanes, max_threshold, outs, 0, nullptr, nullptr, dq);
}

void finish_block16_n_u64(const std::uint64_t* ns,
                          const std::uint64_t* ms,
                          std::size_t count,
                          std::size_t lanes,
                          u128 max_threshold,
                          BlockOutcome* outs,
                          DeferQueue* dq) noexcept {
    // finish_block16_n_impl's persistent fast lane over u64 inputs: same hi,
    // same epoch / scan_needed / folded protocol, same persist spin and queue
    // hand-off via persist_batch, so every outcome and every dq field equals
    // finish_block16_n on the zero-extended arrays.
    const u128 fc = max_threshold >> 10;
    const std::uint64_t hi =
        hi64(fc) != 0 ? lo64(kU64FastBar) : std::min(lo64(fc), lo64(kU64FastBar));
    // A queue needs count <= kBatchMax (asserted in finish_block16_n_impl).
    // Testing it here also lets the compiler treat the batch as one
    // Deferred::kCap chunk in the spin.
    if (dq != nullptr && count <= DeferQueue::kBatchMax && lanes >= 2 && hi > 3) [[likely]] {
        ++dq->epoch;
        dq->scan_needed = true;
        dq->folded = false;
        // The queue is attached, so the inline drain is unreachable.
        persist_batch(ns, ms, count, hi, outs, dq, [](const Deferred&, std::size_t) {});
        return;
    }
    // Otherwise (no record yet, so hi <= 3; lanes < 2; or no queue) the inputs
    // are zero-extended for the u128 driver.
    static thread_local std::vector<u128> wn;
    static thread_local std::vector<u128> wm;
    wn.assign(ns, ns + count);
    wm.assign(ms, ms + count);
    finish_block16_n_impl(
        wn.data(), wm.data(), count, lanes, max_threshold, outs, 0, nullptr, nullptr, dq);
}

void drain_deferred(const DeferQueue& q,
                    std::size_t lanes,
                    u128 max_threshold,
                    BlockOutcome* out) noexcept {
    // Resumes every queued candidate on the block-granular slot driver over full
    // lanes: n = q.n, from q.v with its own block count q.blk, the max seeded from
    // the original m = q.m. The result equals that of the per-batch inline drain;
    // only the grouping differs.
    const std::size_t count = q.size();
    if (count == 0) {
        return;
    }
    finish_block16_n_impl(
        q.n.data(), q.v.data(), count, lanes, max_threshold, out, 0, q.m.data(), q.blk.data());
}

} // namespace collatz::step3
