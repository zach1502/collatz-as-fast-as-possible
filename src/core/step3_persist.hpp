#pragma once

// Persistent u64 fast-lane spin, part 1: the {3^f, c} block table, the deferred
// hand-off set, the lane state and the per-lane refill and retire steps.

#include "collatz/digest_term.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "step3_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace collatz::step3::detail {

/// block_table16 with the multiplier inline: entry = (3^f << 32) | T^16(r), so
/// a block is one load (no f mask, no dependent 3^f load).
///
/// A heap std::vector, unlike the StaticTable tables: p3 and the pair
/// table below feed the register-bound persistent spin, and the link-time-constant
/// address of a static-storage array gives that spin a worse register allocation.
[[nodiscard]] static inline const std::vector<std::uint64_t>& block_table16_p3() {
    static const std::vector<std::uint64_t> table = [] {
        const BlockTable16& base = block_table16();
        std::vector<std::uint64_t> t(base.size());
        for (std::size_t r = 0; r < base.size(); ++r) {
            t[r] = (static_cast<std::uint64_t>(kPow3Low[base[r] & kFMask]) << 32u) |
                   static_cast<std::uint64_t>(base[r] >> 5u);
        }
        return t;
    }();
    return table;
}

/// block_table16_p3 split into a 16-B entry {m = 3^f, c = T^16(r)} so the
/// persistent spin's block is `ldp` + `madd` (no shift or zero-extend of the
/// packed entry). Same m, c and u64 arithmetic as block_table16_p3; 1 MB
/// instead of 512 KB.
struct alignas(16) BlockMC {
    std::uint64_t m;
    std::uint64_t c;
};

[[nodiscard]] static inline const std::vector<BlockMC>& block_table16_mc() {
    static const std::vector<BlockMC> table = [] {
        const std::vector<std::uint64_t>& p3 = block_table16_p3();
        std::vector<BlockMC> t(p3.size());
        for (std::size_t r = 0; r < p3.size(); ++r) {
            t[r] = BlockMC{p3[r] >> 32u, static_cast<std::uint32_t>(p3[r])};
        }
        return t;
    }();
    return table;
}
using PersistTab = BlockMC;

/// Candidates the persistent spin hands back to the slot driver, with their
/// block_step state (v, blocks) at hand-off.
struct Deferred {
    static constexpr std::size_t kCap = kBatchCap;
    u128 n[kCap];
    u128 v[kCap];
    u128 maxv[kCap];
    std::uint64_t blk[kCap];
    std::size_t idx[kCap];
    std::size_t count = 0;
    /// Set by skip_candidate when a candidate is not admitted to the
    /// fast lane at load (n <= 2 base case, or m outside [3, hp) / u128). Only
    /// such an outcome can carry max_seen >= the running record, so a batch with
    /// no load skip lets the caller skip its record-gate scan. Mid-walk
    /// deferrals (persist_retire -> defer_candidate) do not set it: they were
    /// admitted, so their m < hp.
    bool load_skip = false;
    /// Fold-inline: per-call sums of blocks and raw u128 pow-v5 terms over every
    /// candidate the kFold spin retired by descent (and its n <= 2 base cases);
    /// only read/written by the kFold instantiation.
    std::uint64_t fw = 0;
    u128 fp = 0;
};

/// Persistent-lane fast spin: W lanes stay in the u64 fast lane and a lane
/// whose chain retires (v < n) writes its outcome and refills in-register from
/// the next pending candidate (the refilling-lane / persistent-thread pattern),
/// so there is no per-iteration gather, BlockChain load or block_step pass. With
/// maxv = m and a batch threshold `thr`, block_step's fast-lane gate
/// `m < 2^54 && m < max(m, thr) >> 10` is exactly `m < hi` with the batch
/// constant hi = min(2^54, thr >> 10), and bar stays thr in-lane. A candidate
/// that fails the gate at load (n <= 2 is the base case) or a lane that climbs
/// to v >= hi is deferred with its exact state; the caller finishes deferred
/// ones through the slot driver. Per-candidate results are those of
/// finish_block16; blocks = iterations since load because every lane advances
/// once per iteration.
///
/// The persist machinery is templated on the batch element type E: u128
/// (the general batch) or std::uint64_t (a TailBatch whose every n and m fits
/// u64, see finish_block16_n_u64). For E = u64 the high words are zero by
/// construction, so hi_part() is the constant 0 and the fetch gate has no
/// hi-word test; every other operation is the u128 one on the same values.
template <class E>
[[nodiscard]] static constexpr std::uint64_t hi_part([[maybe_unused]] E x) noexcept {
    if constexpr (sizeof(E) == sizeof(u128)) {
        return hi64(x);
    } else {
        return 0;
    }
}

template <class E>
[[gnu::noinline]] static void defer_candidate(
    Deferred& d, const E* ns, const E* ms, std::size_t i, u128 v, std::uint64_t b) noexcept {
    const std::size_t k = d.count++;
    d.n[k] = ns[i];
    d.v[k] = v;
    d.maxv[k] = ms[i];
    d.blk[k] = b;
    d.idx[k] = i;
}

/// A candidate the fast lane does not take at load: the n <= 2 base case, or a
/// start outside the u64 lane (deferred with blocks = 0).
template <bool kFold, class E>
[[gnu::noinline]] static void
skip_candidate(Deferred& d, const E* ns, const E* ms, BlockOutcome* outs, std::size_t i) noexcept {
    d.load_skip = true; // this outcome's max_seen = ms[i] is unbounded
    if (ns[i] <= 2) {
        BlockOutcome& o = outs[i];
        o = BlockOutcome{};
        o.max_seen = ms[i];
        o.v_end = ns[i];
        if constexpr (kFold) {
            // Fold-inline: the base case's (n, blocks = 0, v_end = n) term, the
            // same triple pass 2 would read back from o (blocks adds 0 to fw).
            d.fp += digest::result_digest_term(ns[i], 0, ns[i]);
        }
    } else {
        defer_candidate(d, ns, ms, i, ms[i], 0);
    }
}

/// Lane count of the persistent spin. Lanes live in memory, so W is not bound
/// by kWmax. Results are identical for any W; only the lane interleaving
/// changes.
constexpr std::size_t kPersistW = 24;

/// Persistent-spin lane state, structure of arrays.
template <std::size_t W>
struct PersistState {
    std::uint64_t v[W]{};
    std::uint64_t n64[W]{};
    std::uint64_t st[W]{};
    std::size_t id[W]{};
};

template <class E>
struct PersistCtx {
    using Elem = E;
    const E* ns;
    const E* ms;
    std::size_t count;
    std::uint64_t hi;
    const PersistTab* T;
    BlockOutcome* outs;
    Deferred* d;
};

/// Scalar spin state kept apart from the lane arrays so it stays in registers.
struct PersistCursor {
    static constexpr bool kFold = false;
    std::size_t next = 0;
    std::size_t nlive = 0;
    std::uint64_t it = 0;
};

/// Fold-inline cursor: also carries the per-call block and raw u128 pow-v5
/// sums, touched only when a lane retires by descent (off the block-step
/// dependency chain). Flushed to Deferred at the end of persist_run.
struct PersistCursorFold : PersistCursor {
    static constexpr bool kFold = true;
    std::uint64_t fw = 0;
    u128 fp = 0;
};

template <std::size_t W, class C, class X>
[[gnu::always_inline]] static inline void
persist_fetch(PersistState<W>& s, C& c, const X& x, std::size_t j) noexcept {
    for (;;) {
        if (c.next >= x.count) [[unlikely]] {
            s.v[j] = 0; // parked: T^16 maps 0 to 0 and 0 - 0 never reads as an exit
            s.n64[j] = 0;
            --c.nlive;
            return;
        }
        const std::size_t i = c.next++;
        using E = typename X::Elem;
        const E n = x.ns[i];
        const E m = x.ms[i];
        const auto nl = static_cast<std::uint64_t>(n);
        const auto ml = static_cast<std::uint64_t>(m);
        // == n > 2 && n <= m && m < hi, given 3 < hi <= 2^54 (hi_part == 0 for E = u64).
        const bool ok = ((hi_part(n) | hi_part(m)) == 0) & (ml - 3 < x.hi - 3) & (nl - 3 <= ml - 3);
        if (ok) [[likely]] {
            s.v[j] = ml;
            s.n64[j] = nl;
            s.id[j] = i;
            s.st[j] = c.it;
            __builtin_prefetch(&x.T[ml & 0xFFFFu], 0, 3); // table entry of the first block
            if constexpr (!C::kFold) {
                x.outs[i].max_seen = ml; // a deferred lane's outcome is overwritten later
                x.outs[i].overflow = false;
            }
            return;
        }
        skip_candidate<C::kFold>(*x.d, x.ns, x.ms, x.outs, i);
    }
}

/// Fold-inline: credit one descended lane's (n, blocks, v_end) to the cursor
/// sums instead of storing blocks / v_end to outs. n and v_end are the lane's u64
/// values with zero high halves (the admit test requires hi64(n) == 0 and v < n),
/// so result_digest_term(n, b, v) is exactly the term pass 2 would compute from
/// the outs it does not read; the sums are order-free and kept raw in u128
/// like pass 2's loc_pow_raw.
template <std::size_t W>
[[gnu::always_inline]] static inline void
persist_fold(PersistState<W>& s, PersistCursorFold& c, std::size_t j, std::uint64_t b) noexcept {
    c.fw += b;
    c.fp += digest::result_digest_term(s.n64[j], b, s.v[j]);
}

/// Retires a lane whose v < n (the exit mask holds only such lanes).
template <std::size_t W, class C, class X>
[[gnu::always_inline]] static inline void
persist_descend(PersistState<W>& s, C& c, const X& x, std::size_t j) noexcept {
    if constexpr (C::kFold) {
        persist_fold(s, c, j, c.it - s.st[j]);
    } else {
        BlockOutcome& o = x.outs[s.id[j]];
        o.blocks = c.it - s.st[j];
        o.v_end = s.v[j];
    }
    persist_fetch(s, c, x, j);
}

template <std::size_t W, class C, class X>
[[gnu::always_inline]] static inline void
persist_retire(PersistState<W>& s, C& c, const X& x, std::size_t j) noexcept {
    const std::uint64_t b = c.it - s.st[j];
    const std::size_t k = s.id[j];
    if (s.v[j] < s.n64[j]) [[likely]] {
        if constexpr (C::kFold) {
            persist_fold(s, c, j, b);
        } else {
            BlockOutcome& o = x.outs[k];
            o.blocks = b;
            o.v_end = s.v[j]; // max_seen/overflow already set by persist_fetch
        }
    } else {
        defer_candidate(*x.d, x.ns, x.ms, k, s.v[j], b);
    }
    persist_fetch(s, c, x, j);
}

} // namespace collatz::step3::detail
