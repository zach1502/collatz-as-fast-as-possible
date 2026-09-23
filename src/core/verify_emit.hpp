#ifndef COLLATZ_CORE_VERIFY_EMIT_HPP
#define COLLATZ_CORE_VERIFY_EMIT_HPP

// Candidate emission into a thread's TailBatch: push_tail_batch adds one
// candidate, emit_batched_u64 adds every candidate of a Step-2 mask whose n
// and m_a fit u64.

#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "verify_internal.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

// always_inline on a lambda's call operator: clang takes the GNU attribute after
// the parameter list (and rejects it before), g++ takes it after the capture
// list (and rejects the post-parameter form as "does not apply to types").
// [[gnu::always_inline]] is ignored as a type attribute by both in C++20.
#if defined(__clang__)
#define COLLATZ_LAMBDA_AI_PRE
#define COLLATZ_LAMBDA_AI_POST __attribute__((always_inline))
#else
#define COLLATZ_LAMBDA_AI_PRE __attribute__((always_inline))
#define COLLATZ_LAMBDA_AI_POST
#endif

namespace collatz::verify::detail {

/// Append one table16 candidate to a per-thread buffer, flushing when it fills
/// to capacity. The buffer spans survivors, so trailing entries are flushed
/// after the Step-1 join.
inline void push_tail_batch(Acc& acc,
                            TailBatch& b,
                            unsigned M,
                            std::size_t lanes,
                            u128 n,
                            u128 m,
                            u128 pmax,
                            u128 smax,
                            unsigned a,
                            bool has_prefix) {
    const std::size_t i = b.count;
    if (b.lo && (hi64(n) | hi64(m)) == 0) {
        b.n64[i] = lo64(n);
        b.m64[i] = lo64(m);
    } else {
        b.widen(); // a high word: the rest of this fill is u128
        b.n[i] = n;
        b.m[i] = m;
    }
    if (has_prefix) {
        // pmax/smax/a feed prefix_ub in finalize only for prefix-relevant
        // candidates; skip the u128 stores otherwise. hp stays all-false
        // for the rest (TailBatch::any_hp), so store only the true entries.
        b.hp[i] = true;
        b.any_hp = true;
        b.pmax[i] = pmax;
        b.smax[i] = smax;
        b.a[i] = a;
    }
    ++b.count;
    if (b.count >= kTailBatchCap) {
        flush_tail_batch(acc, b, M, lanes);
    }
}

/// The emit call's run invariants, passed by reference so the call's
/// arguments fit in registers. The flush arguments are read from here at the
/// (rare) flush, so they are not live across the push loop.
struct EmitCtx {
    std::uint64_t preflimit_lo = 0;
    std::size_t lanes = 1;
    unsigned twoA = 1;  ///< 2^A: extensions per survivor.
    unsigned nodeM = 0; ///< M: candidate a is n0 + (a << M).
};
/// Per-survivor emit inputs (written once per survivor on the stack).
/// No default initializers, so the driver stores only the fields the chosen
/// instantiation reads.
struct EmitSurv {
    std::uint64_t n0_lo;
    std::uint64_t m_lo;
    std::uint64_t p3f_lo;
    u128 pmax; ///< written/read only for the kPrefix instantiations
    u128 smax; ///< written/read only for the kPrefix instantiations
};

/// Common-case candidate emission: n and m_a fit u64. Pushes exactly the
/// records SurvivorProcessor::emit_generic would.
/// Scans from word wi0 and folds the survivor's coverage and masked_out.
template <bool kPrefix>
[[gnu::noinline]] void emit_batched_u64(Acc& acc,
                                        TailBatch& batch,
                                        const step2::CandidateMask& mask,
                                        const EmitCtx& ctx,
                                        const EmitSurv& sv,
                                        unsigned wi0) {
    const unsigned twoA = ctx.twoA;
    const std::uint64_t n0_lo = sv.n0_lo;
    const std::uint64_t m_lo = sv.m_lo;
    const std::uint64_t p3f_lo = sv.p3f_lo;
    const std::uint64_t preflimit_lo = ctx.preflimit_lo;
    const unsigned nodeM = ctx.nodeM;
    [[maybe_unused]] const u128 pmax = kPrefix ? sv.pmax : u128{0};
    [[maybe_unused]] const u128 smax = kPrefix ? sv.smax : u128{0};
    // Same bound as step3/block.hpp (kBatchCap >= 64). At exactly 64 the per-word
    // unchecked loop below is never taken (cnt < cap - 64 is cnt < 0), so every
    // push goes through the checked cap test: still exact.
    static_assert(kTailBatchCap >= 64, "the tail batch must hold a full mask word");
    u128 cov = 0;
    std::uint64_t pushed = 0;
    std::uint64_t skipped = 0;
    // Batch fill level kept in a register; b.count is only synced around flushes.
    std::size_t cnt = batch.count;
    // The mask is read-only here and flush_tail_batch cannot reach it (it is
    // the caller's per-survivor local), but through the const& the compiler
    // must reload mask.words / &mask.w[0] after every out-of-line flush and
    // every store it cannot disambiguate, so both are held in locals.
    const std::uint64_t* const W = mask.w.data();
    const unsigned words = mask.words;
    // Preflight as a bit-prefix clear. On this path n_lo(a) = n0_lo +
    // (a << nodeM) never wraps u64 (n < 2^N <= 2^64, nodeM <= 63 since A >= 1;
    // see run()), so n_lo is strictly increasing in a and the per-candidate test
    // n_lo < preflimit_lo holds exactly for a < a_min, where, with
    // d = preflimit_lo - n0_lo > 0, a * 2^nodeM < d <=> a < ceil(d / 2^nodeM).
    // The ceil is computed as (d >> nodeM) + (remainder != 0) so it cannot
    // overflow, then clamped to twoA. Those set bits are counted into `skipped`
    // exactly as a per-candidate `continue` would count them, so masked_out is
    // the same.
    unsigned a_min = 0;
    if (n0_lo < preflimit_lo) {
        const std::uint64_t d = preflimit_lo - n0_lo;
        const std::uint64_t rem = d & ((std::uint64_t{1} << nodeM) - 1u);
        const std::uint64_t q = (d >> nodeM) + (rem != 0 ? 1u : 0u);
        a_min = q >= twoA ? twoA : static_cast<unsigned>(q);
    }
    // Push one set bit `a`. `checked` selects the per-candidate cap test; the
    // unchecked form is only used when the whole word cannot fill the batch.
    const auto emit_one = [&] COLLATZ_LAMBDA_AI_PRE(unsigned a,
                                                    bool checked) COLLATZ_LAMBDA_AI_POST {
        const std::uint64_t n_lo = n0_lo + (static_cast<std::uint64_t>(a) << nodeM);
        const std::uint64_t ma = m_lo + p3f_lo * static_cast<std::uint64_t>(a);
        cov += mix64(n_lo);
        ++pushed;
        batch.n64[cnt] = n_lo; // one 8-B store per column
        batch.m64[cnt] = ma;
        if constexpr (kPrefix) {
            // hp stays all-false for kPrefix==false entries
            // (TailBatch::any_hp), so the common path skips the per-candidate
            // hp store.
            batch.hp[cnt] = true;
            batch.any_hp = true;
            batch.pmax[cnt] = pmax;
            batch.smax[cnt] = smax;
            batch.a[cnt] = a;
        }
        ++cnt;
        if (checked && cnt == kTailBatchCap) {
            batch.count = cnt;
            flush_tail_batch(acc, batch, ctx.nodeM, ctx.lanes);
            cnt = 0;
        }
    };
    unsigned wi = wi0;
    // Whole words below a_min are all preflight skips (bits below wi0 are zero
    // by the caller's scan, so starting at wi0 counts every one).
    for (; wi < words && wi < (a_min >> 6); ++wi) {
        skipped += static_cast<std::uint64_t>(std::popcount(W[wi]));
    }
    for (; wi < words; ++wi) {
        std::uint64_t bits = W[wi];
        // The one word straddling a_min: clear (and count) its low bits.
        if (wi == (a_min >> 6) && (a_min & 63u) != 0) {
            const std::uint64_t low = bits & ((std::uint64_t{1} << (a_min & 63u)) - 1u);
            skipped += static_cast<std::uint64_t>(std::popcount(low));
            bits ^= low;
        }
        const unsigned base = wi * 64u;
        // A word holds <= 64 set bits, so with cnt < cap - 64 the word
        // leaves cnt <= cap - 1 and the batch cannot fill inside it: the checked
        // loop would never flush there, and skipping the test keeps the flush
        // points identical. Strict `<` (not `<=`) so a 64-bit word can never
        // leave a full, un-flushed batch behind.
        if (cnt < kTailBatchCap - 64) {
            while (bits != 0) {
                const unsigned a = base + static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                emit_one(a, false);
            }
        } else {
            while (bits != 0) {
                const unsigned a = base + static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                emit_one(a, true);
            }
        }
    }
    batch.count = cnt;
    acc.coverage_raw += cov;
    // Every set bit is a preflight skip or exactly one candidate.
    acc.masked_out += twoA - (pushed + skipped);
}

} // namespace collatz::verify::detail

#endif // COLLATZ_CORE_VERIFY_EMIT_HPP
