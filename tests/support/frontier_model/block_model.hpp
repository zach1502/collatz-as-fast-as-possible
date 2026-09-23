#ifndef COLLATZ_TESTS_FRONTIER_MODEL_BLOCK_MODEL_HPP
#define COLLATZ_TESTS_FRONTIER_MODEL_BLOCK_MODEL_HPP

// The block model (Model, block_model), the exact max over a range of numbers
// (Brute, brute_force) and the checks of a block Result against both.
// frontier_model.hpp includes this file.

#include "collatz/collatz_fn.hpp"
#include "collatz/linear.hpp"
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "collatz_ref/ref_step2.hpp"
#include "collatz_ref/ref_step3.hpp"
#include "frontier_model/walk.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace collatz::frontier_test {

struct Model {
    std::uint64_t candidates = 0;
    std::uint64_t blocks = 0;
    u128 cov_raw = 0;
    u128 pow_raw = 0;
    std::string max_dec = "0";
    u128 argmax = 0;
    std::uint64_t wide = 0;
    std::vector<std::pair<u128, std::string>> peaks; ///< every candidate (n, peak)
    std::vector<u128> cands;                         ///< sorted
};

/// The block candidate set and Step-3 folds built independently of
/// verify.cpp / step1.cpp / step2.cpp / step3_*.cpp (window terms off).
inline Model block_model(unsigned K, u128 H, unsigned A, unsigned B, unsigned nbv) {
    const unsigned M = K - A;
    const u128 L = H << K;
    collatz::ref::Options ro;
    ro.mod3t = 6;
    static std::map<unsigned, std::vector<collatz::ref::Survivor>> surv_cache;
    std::vector<collatz::ref::Survivor>& surv = surv_cache[M];
    if (surv.empty()) {
        surv = collatz::ref::survivors(M, ro); // h-independent: one tree per depth
    }
    static std::map<unsigned, std::vector<std::uint32_t>> dip_cache;
    std::vector<std::uint32_t>& dip = dip_cache[B];
    if (dip.empty()) {
        dip.resize(std::size_t{1} << B);
        for (std::uint64_t r = 0; r < dip.size(); ++r) {
            dip[r] = collatz::ref::dip_B(r, B);
        }
    }
    const unsigned fmin = collatz::ref::fmin_for(K, A);
    const std::uint64_t bmask = (std::uint64_t{1} << B) - 1;
    Model md;
    for (const collatz::ref::Survivor& s : surv) {
        const u128 n0 = s.n0 + L;
        u128 m = n0;
        unsigned f = 0;
        for (unsigned j = 0; j < M; ++j) {
            f += static_cast<unsigned>(m & 1u);
            m = collatz::T(m).value;
        }
        REQUIRE(f == s.f); // the parity word of the first M steps is h-independent
        for (std::uint32_t a = 0; a < (1u << A); ++a) {
            const u128 n = n0 + (static_cast<u128>(a) << M);
            const unsigned r9 = static_cast<unsigned>(n % 9);
            if (r9 == 2 || r9 == 4 || r9 == 5 || r9 == 8) {
                continue;
            }
            const u128 m_a = m + collatz::pow3(f) * static_cast<u128>(a);
            if (f < fmin + nbv) {
                const long thr = 485L * static_cast<long>(f) - 306L * static_cast<long>(M);
                if (static_cast<long>(dip[lo64(m_a) & bmask]) >= thr) {
                    continue;
                }
            }
            if (oee_excluded(n, M)) {
                continue;
            }
            const Walk w = walk_candidate(n, M);
            REQUIRE(w.m == m_a); // the affine lift agrees with the direct walk
            ++md.candidates;
            md.blocks += w.blocks;
            md.cov_raw += cov_term(n);
            md.pow_raw += collatz::ref::result_digest_term(n, w.blocks, w.v_end);
            md.wide += w.wide ? 1u : 0u;
            if (collatz::decimal_less(md.max_dec, w.peak) ||
                (w.peak == md.max_dec && n < md.argmax)) {
                md.max_dec = w.peak;
                md.argmax = n;
            }
            md.peaks.emplace_back(n, w.peak);
            md.cands.push_back(n);
        }
    }
    std::sort(md.cands.begin(), md.cands.end());
    std::sort(md.peaks.begin(), md.peaks.end());
    return md;
}

/// Exact max of Q(n) = max until the first drop below n, over [lo, hi), smallest
/// argmax on ties (linear::verify_one_detailed, GMP on overflow).
struct Brute {
    std::string max_dec = "0";
    u128 argmax = 0;
};

inline Brute brute_force(u128 lo, u128 hi) {
    // Ascending scan with strict '>' keeps the smallest argmax on ties. A peak
    // above 2^128-1 (GMP oracle) dominates every u128 peak.
    u128 best = 0;
    u128 best_n = 0;
    bool have = false;
    std::string wide;
    u128 wide_n = 0;
    for (u128 n = lo; n < hi; ++n) {
        const collatz::linear::OneResult r = collatz::linear::verify_one_detailed(n);
        u128 x = r.max_seen;
        if (r.overflowed) {
            mpz_class mx;
            (void) collatz::linear::verify_one_oracle(n, mx);
            const std::string d = collatz::oracle::to_decimal(mx);
            if (mpz_sizeinbase(mx.get_mpz_t(), 2) > 128) {
                if (wide.empty() || collatz::decimal_less(wide, d)) {
                    wide = d;
                    wide_n = n;
                }
                continue;
            }
            REQUIRE(collatz::from_string(d, x));
        }
        if (!have || x > best) {
            best = x;
            best_n = n;
            have = true;
        }
    }
    Brute b;
    b.max_dec = wide.empty() ? collatz::to_string(best) : wide;
    b.argmax = wide.empty() ? best_n : wide_n;
    return b;
}

inline void check_against_model(const v::Result& r, const Model& md) {
    CHECK(r.candidates_checked == md.candidates);
    CHECK(r.coverage_checksum == static_cast<std::uint64_t>(md.cov_raw % kP));
    CHECK(r.pow_checksum == static_cast<std::uint64_t>(md.pow_raw % kP));
    CHECK(r.blocks_total == md.blocks);
    CHECK(v::max_excursion_decimal(r) == md.max_dec);
    CHECK(r.max_excursion_n == md.argmax);
    CHECK(r.preflight_candidates == 0);
    CHECK(r.overflow_fallbacks >= md.wide);
}

inline void check_against_brute(const v::Result& r, const Model& md, u128 lo, u128 hi) {
    const Brute bf = brute_force(lo, hi);
    const std::string R = v::max_excursion_decimal(r);
    // R is the largest excursion over the walked members, and the brute force
    // takes the same excursion over every member, so R is at most its maximum.
    CHECK_FALSE(collatz::decimal_less(bf.max_dec, R));
    if (std::binary_search(md.cands.begin(), md.cands.end(), bf.argmax)) {
        CHECK(R == bf.max_dec);
        CHECK(r.max_excursion_n == bf.argmax);
    }
    const bool walked = std::binary_search(md.cands.begin(), md.cands.end(), bf.argmax);
    MESSAGE("block max " << R << " @ " << collatz::to_string(r.max_excursion_n)
                         << "; brute-force block max " << bf.max_dec << " @ "
                         << collatz::to_string(bf.argmax)
                         << std::string(walked ? " (walked)"
                                               : " (excluded: on a smaller number's orbit)"));
}

} // namespace collatz::frontier_test

#endif // COLLATZ_TESTS_FRONTIER_MODEL_BLOCK_MODEL_HPP
