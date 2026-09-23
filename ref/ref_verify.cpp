// Reference end-to-end verifier. See ref/include/collatz_ref/ref_verify.hpp.
// Composes ref::candidates (Step 1+2) with ref::finish (Step 3) and aggregates
// the same result scalars as verify::Result. It shares no code with
// src/core/verify.cpp.

#include "collatz_ref/ref_verify.hpp"

#include "collatz_ref/ref_step2.hpp"
#include "collatz_ref/ref_step3.hpp"

#include <cstdint>

namespace collatz::ref {
namespace {

/// The preflight (see the verify.hpp file comment) verifies every n in [1, 2^17)
/// with an exact walk and folds each into the same digests as a checked
/// candidate: it finishes the number from itself (m = n, the "verify one"
/// computation), so its 16-step block count and landing value contribute to
/// blocks_total and the pow digest. This contribution is independent of N, so
/// it is computed once.
struct Preflight {
    std::uint64_t blocks_total = 0;
    collatz::u128 pow_raw = 0;
    collatz::u128 cov_raw = 0;
    std::uint64_t count = 0;
};

[[nodiscard]] const Preflight& preflight_contribution() {
    static const Preflight cached = [] {
        Preflight p;
        for (collatz::u128 n = 1; n < kPreflightBound; ++n) {
            const BlockResult br = ref::finish_blocks(n, n);
            p.blocks_total += br.blocks;
            p.pow_raw += static_cast<collatz::u128>(br.pow_term);
            p.cov_raw += static_cast<collatz::u128>(mix64(collatz::lo64(n)) ^
                                                    (collatz::hi64(n) * 0x9E3779B97F4A7C15ULL));
            ++p.count;
        }
        return p;
    }();
    return cached;
}

} // namespace

VerifyResult verify_ref(unsigned N,
                        unsigned A,
                        unsigned B,
                        unsigned num_bitvectors,
                        bool partial_oee,
                        const Options& step1_opts,
                        unsigned window_chains,
                        unsigned window_templates_D) {
    VerifyResult res;

    const auto cands = ref::candidates(
        N, A, B, num_bitvectors, partial_oee, step1_opts, window_chains, window_templates_D);

    // Seed the digests with the fixed preflight contribution (n in [1, 2^17)).
    const Preflight& pre = preflight_contribution();
    collatz::u128 pow_raw = pre.pow_raw;
    collatz::u128 cov_raw = pre.cov_raw;
    res.blocks_total = pre.blocks_total;

    for (const auto& c : cands) {
        // Candidates with n < 2^17 are discharged by the preflight above and are
        // not re-counted here (see ref_verify.hpp preflight note).
        if (c.n < kPreflightBound) {
            continue;
        }
        const Finish fin = ref::finish(c.n, c.m_a);
        ++res.checked_candidates;
        res.blocks_total += fin.blocks; // wrapping u64 diagnostic.
        pow_raw += static_cast<collatz::u128>(fin.pow_term);
        // Coverage: the mod-2^61-1 set-fold over every verified n, matching
        // coverage_checksum term for term.
        cov_raw += static_cast<collatz::u128>(mix64(collatz::lo64(c.n)) ^
                                              (collatz::hi64(c.n) * 0x9E3779B97F4A7C15ULL));
        if (fin.max_exceeds_u128) {
            res.max_exceeds_u128 = true;
        }
        // Track the global maximum, smallest candidate n on ties. Candidates
        // have n >= 2^17 > 0, so max_excursion_n == 0 marks "no candidate yet".
        const bool first = (res.max_excursion_n == 0);
        if (first || fin.max_excursion > res.max_excursion ||
            (fin.max_excursion == res.max_excursion && c.n < res.max_excursion_n)) {
            res.max_excursion = fin.max_excursion;
            res.max_excursion_n = c.n;
        }
    }

    res.candidates_checked = kPreflightCount + res.checked_candidates;
    res.pow_v4 = static_cast<std::uint64_t>(pow_raw % kFieldP);
    res.coverage = static_cast<std::uint64_t>(cov_raw % kFieldP);
    return res;
}

} // namespace collatz::ref
