#ifndef COLLATZ_CORE_VERIFY_SURVIVOR_HPP
#define COLLATZ_CORE_VERIFY_SURVIVOR_HPP

// SurvivorProcessor, run()'s per-survivor Step-2 mask and candidate emission.
// Its only user is the Step-1 sink in verify_step2.cpp, which inlines it for
// every survivor.

#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_emit.hpp"
#include "verify_fold.hpp"
#include "verify_internal.hpp"

#include <cstdint>

namespace collatz::verify::detail {

/// Whether m + p3f * amax is below 2^64, i.e. every m_a = m + p3f * a with
/// a <= amax fits u64. Each operation has its own overflow flag: a product past
/// u128 can wrap to a value whose sum with m does not carry.
[[nodiscard]] inline bool madd_fits_u64(u128 m, u128 p3f, unsigned amax) noexcept {
    bool mul_overflow = false;
    bool add_overflow = false;
    const u128 top =
        checked_add(m, checked_mul(p3f, static_cast<u128>(amax), mul_overflow), add_overflow);
    return !mul_overflow && !add_overflow && hi64(top) == 0;
}

/// The A > 0 per-survivor step of run(): the Step-2 mask, then every
/// surviving extension a (candidate n = n0 + a*2^M) to the Step-3 tail. The
/// members are run invariants.
struct SurvivorProcessor {
    const step2::Filter& filter;
    const EmitCtx& emit_ctx;
    u128 preflimit;
    std::size_t lanes;
    unsigned twoA;
    unsigned M;
    bool values_fit_u64;

    void operator()(Acc& acc, TailBatch& batch, const step1::Node& node) const {
        const step2::CandidateMask mask = filter.mask(node);
        unsigned wi0 = 0;
        while (wi0 < mask.words && mask.w[wi0] == 0) {
            ++wi0;
        }
        if (wi0 == mask.words) {
            acc.masked_out += twoA;
            return;
        }
        // All 2^A extensions of this survivor share n0, m, f and the prefix
        // bound (node.pmax, node.smax) Step 1 carried down the DFS, so there
        // is no per-survivor prefix walk.
        const u128 pmax = node.pmax;
        const u128 smax = node.smax;
        const u128 p3f = pow3(node.f);
        // Per-survivor prefix relevance. All 2^A extensions share
        // (pmax, smax), so the survivor's largest possible prefix upper
        // bound is prefix_ub(pmax, smax, 2^A - 1). If it is already below the
        // running record it can never contend (records only grow between
        // here and the batch flush), so every extension is pushed
        // has_prefix=false, skipping the pmax/smax stores and the
        // per-candidate prefix_ub u128 multiply in finalize. Sound because
        // the exact record update still consults tail_ge, and a below-record
        // prefix is strictly below the (only larger) flush-time record.
        const bool prefix_rel = !acc.have_max || prefix_ub(pmax, smax, twoA - 1) >= acc.bar;
        // u64 fast lane for m_a = m + 3^f * a. For N <= 64, m_a fits u64
        // iff m + 3^f*(2^A - 1) < 2^64; when it does, one u64 madd replaces
        // the u128 checked_mul + checked_add. The flag is exact, so the rare
        // survivor whose m_a can exceed 2^64 (large M) still takes the u128
        // overflow-checked path, and the N > 64 contract holds.
        const std::uint64_t p3f_lo = lo64(p3f);
        const std::uint64_t m_lo = lo64(node.m);
        const std::uint64_t n0_lo = lo64(node.n0); // all of n0 when n_fits_u64
        // When the run's numbers reach 2^64 (N > 64, or a block whose
        // value width passes 64 bits) the candidate n = n0 + a*2^M exceeds
        // u64, so build it full-width and drop the u64 shift/compare fast
        // path. Otherwise hi64(n) == 0 and the u64 path is exact.
        const bool n_fits_u64 = values_fit_u64;
        const bool ma_u64 = madd_fits_u64(node.m, p3f, twoA - 1);
        if (n_fits_u64 && ma_u64) {
            if (!batch.lo) [[unlikely]] {
                // A u128 push widened this fill (m >= 2^64, or N > 64); flush
                // it so emit writes the u64 columns. Bit-identical: Results do
                // not depend on batch boundaries.
                flush_tail_batch(acc, batch, M, lanes);
            }
            const auto emit = prefix_rel ? &emit_batched_u64<true> : &emit_batched_u64<false>;
            EmitSurv sv;
            sv.n0_lo = n0_lo;
            sv.m_lo = m_lo;
            sv.p3f_lo = p3f_lo;
            if (prefix_rel) {
                sv.pmax = pmax;
                sv.smax = smax;
            }
            emit(acc, batch, mask, emit_ctx, sv, wi0);
            return;
        }
        emit_generic(acc, batch, node, mask, p3f, ma_u64, prefix_rel);
    }

    // Every survivor the u64 batch path above does not take: n or m_a past
    // 2^64.
    void emit_generic(Acc& acc,
                      TailBatch& batch,
                      const step1::Node& node,
                      const step2::CandidateMask& mask,
                      u128 p3f,
                      bool ma_u64,
                      bool prefix_rel) const {
        const u128 pmax = node.pmax;
        const u128 smax = node.smax;
        const std::uint64_t p3f_lo = lo64(p3f);
        const std::uint64_t m_lo = lo64(node.m);
        const std::uint64_t n0_lo = lo64(node.n0);
        const std::uint64_t preflimit_lo = lo64(preflimit);
        const bool n_fits_u64 = values_fit_u64;
        acc.masked_out += twoA - static_cast<std::uint64_t>(mask.popcount());
        // Fold the coverage checksum for batched candidates here (it depends
        // only on n, known at push time) into a survivor-local sum, added to
        // acc once below. This keeps mix64 and its two rematerialized 64-bit
        // multiply constants out of the flush loop.
        u128 surv_cov = 0;
        for (unsigned wi = 0; wi < mask.words; ++wi) {
            std::uint64_t bits = mask.w[wi];
            const unsigned base = wi * 64u;
            while (bits != 0) {
                const unsigned a = base + static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                // n = n0 + a*2^M < 2^N <= 2^64 for N <= 64 (and M <= 63
                // here since A >= 1), so one u64 shift+add builds it in place
                // of the variable-width u128 shift. hi64(n) == 0, so the
                // coverage key and every later use match the u128 form.
                // For N > 64, n is built full-width (u128).
                u128 n = 0;
                if (n_fits_u64) {
                    const std::uint64_t n_lo = n0_lo + (static_cast<std::uint64_t>(a) << M);
                    if (n_lo < preflimit_lo) {
                        continue; // already discharged by the preflight
                    }
                    n = static_cast<u128>(n_lo);
                } else {
                    n = node.n0 + (static_cast<u128>(a) << M);
                    if (n < preflimit) {
                        continue; // already discharged by the preflight
                    }
                }
                u128 m_a = 0;
                if (ma_u64) {
                    // u64 fast lane (see ma_u64 above): one madd, no u128.
                    // NOLINTNEXTLINE(bugprone-misplaced-widening-cast): ma_u64 bounds it below 2^64
                    m_a = static_cast<u128>(m_lo + p3f_lo * static_cast<std::uint64_t>(a));
                } else {
                    bool overflow = false;
                    const u128 term = checked_mul(p3f, static_cast<u128>(a), overflow);
                    if (!overflow) {
                        m_a = checked_add(node.m, term, overflow);
                    }
                    if (overflow) {
                        // m_a beyond u128 (unreachable for N <= 64): GMP path.
                        process_candidate(acc, n, m_a, /*m_overflowed=*/true);
                        continue;
                    }
                }
                // Coverage key = lo64(n) ^ rotl(hi64(n),32) = n_lo for
                // N <= 64 (hi64(n) == 0); carries the high limb for
                // N>64. Folded here, not in the flush.
                surv_cov += mix64(lo64(n) ^ rotl64(hi64(n), 32));
                push_tail_batch(acc, batch, M, lanes, n, m_a, pmax, smax, a, prefix_rel);
            }
        }
        // Fold this survivor's batched-candidate coverage in one add.
        acc.coverage_raw += surv_cov;
    }
};

} // namespace collatz::verify::detail

#endif // COLLATZ_CORE_VERIFY_SURVIVOR_HPP
