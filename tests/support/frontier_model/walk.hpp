#ifndef COLLATZ_TESTS_FRONTIER_MODEL_WALK_HPP
#define COLLATZ_TESTS_FRONTIER_MODEL_WALK_HPP

// Block run options (shipped, plain), the coverage fold term and the walks of one
// number: excursion_dec is the excursion from its definition, walk_candidate
// walks a candidate as the pipeline does, and oee_excluded applies the
// per-extension partial OEE. frontier_model.hpp includes this file.

#include "collatz/collatz_fn.hpp"
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz_ref/ref_step3.hpp"
#include "mpz_doctest.hpp"
#include "test_env.hpp"

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <string>

namespace collatz::frontier_test {

namespace v = collatz::verify;
using collatz::hi64;
using collatz::lo64;
using collatz::u128;

inline constexpr std::uint64_t kP = (std::uint64_t{1} << 61) - 1;

inline u128 pow2(unsigned k) {
    return static_cast<u128>(1) << k;
}

/// Shipped CLI configuration (cmd_verify_defaults.cpp) at look-ahead width B.
inline v::Options shipped(unsigned K, u128 H, unsigned B, unsigned threads = 4) {
    v::Options o;
    o.N = K;
    o.block_h = H;
    o.A = 10;
    o.B = B;
    o.num_bitvectors = 8;
    o.threads = threads;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.bv_cache = collatz::test::scoped_bv_cache();
    return o;
}

/// Same, without the window chain and template terms, so the model's dip is
/// the plain ref::dip_B (the terms are h-independent; the lift only moves the
/// index).
inline v::Options plain(unsigned K, u128 H, unsigned B, unsigned threads = 4) {
    v::Options o = shipped(K, H, B, threads);
    o.window_chains = 0;
    o.window_templates_D = 0;
    return o;
}

inline std::uint64_t rotl64(std::uint64_t x, unsigned r) {
    return (x << r) | (x >> (64u - r));
}

/// Must match the coverage fold key of src/core/verify_fold.hpp.
inline std::uint64_t cov_term(u128 n) {
    return collatz::ref::mix64(lo64(n) ^ rotl64(hi64(n), 32));
}

inline mpz_class T_mpz(const mpz_class& x) {
    return mpz_even_p(x.get_mpz_t()) ? mpz_class(x / 2) : mpz_class((3 * x + 1) / 2);
}

/// n's excursion from the definition, decimal: the largest T^j(n) before the
/// first value below n (n itself for n <= 1). u128 steps with an explicit
/// overflow test, then GMP from n when a step would leave u128.
inline std::string excursion_dec(u128 n) {
    if (n <= 1) {
        return collatz::to_string(n);
    }
    u128 v = n;
    u128 mx = n;
    for (;;) {
        if ((v & 1u) != 0 && v > (collatz::u128_max - 1) / 3) {
            break; // 3v + 1 needs more than 128 bits
        }
        v = (v & 1u) != 0 ? (3 * v + 1) / 2 : v / 2;
        if (v < n) {
            return collatz::to_string(mx);
        }
        mx = v > mx ? v : mx;
    }
    const mpz_class start = collatz::oracle::to_mpz(n);
    mpz_class x = start;
    mpz_class big = start;
    do {
        x = T_mpz(x);
        big = x > big ? x : big;
    } while (x >= start);
    return collatz::oracle::to_decimal(big);
}

/// One candidate as the pipeline walks and reports it. m = T^M(n), and
/// blocks / v_end come from the pipeline's walk: the M-step prefix from n, then
/// 16-step blocks until a block boundary lands below n. That walk tests "< n"
/// only at block ends, so it can pass the first drop below n; `peak` is the
/// excursion the pipeline records (excursion_dec), decimal.
struct Walk {
    u128 m = 0;
    std::uint64_t blocks = 0;
    u128 v_end = 0;
    std::string peak;
    bool wide = false; ///< the walk needed more than u128 somewhere
};

inline Walk walk_candidate(u128 n, unsigned M) {
    Walk w;
    w.peak = excursion_dec(n);
    u128 v = n;
    bool ovf = false;
    for (unsigned j = 0; j < M && !ovf; ++j) {
        const collatz::StepResult s = collatz::T(v);
        ovf = s.overflow;
        v = s.value;
    }
    w.m = v;
    while (!ovf && v >= n) {
        for (unsigned i = 0; i < 16 && !ovf; ++i) {
            const collatz::StepResult s = collatz::T(v);
            ovf = s.overflow;
            v = s.value;
        }
        ++w.blocks;
    }
    if (!ovf) {
        w.v_end = v;
        return w;
    }
    const mpz_class start = collatz::oracle::to_mpz(n);
    mpz_class V = start;
    mpz_class MX = start;
    for (unsigned j = 0; j < M; ++j) {
        V = T_mpz(V);
        MX = V > MX ? V : MX;
    }
    REQUIRE(mpz_sizeinbase(V.get_mpz_t(), 2) <= 128);
    REQUIRE(collatz::from_string(collatz::oracle::to_decimal(V), w.m));
    w.blocks = 0;
    while (V >= start) {
        for (unsigned i = 0; i < 16; ++i) {
            V = T_mpz(V);
            MX = V > MX ? V : MX;
        }
        ++w.blocks;
    }
    REQUIRE(collatz::from_string(collatz::oracle::to_decimal(V), w.v_end)); // V < n
    w.wide = mpz_sizeinbase(MX.get_mpz_t(), 2) > 128;
    return w;
}

/// Per-extension partial OEE on the concrete number: an odd m_j, j < M, whose
/// odd run is followed by two even steps inside [0, M), with positive slope
/// 2^(j+1) > 3^(f_j), and (m_j - 1)/2 < n. (Non-positive-slope arms never fire
/// on a Step-1 survivor; production records only positive-slope arms.)
inline bool oee_excluded(u128 n, unsigned M) {
    // M <= 76 in any block the tests run (validate_block); fixed arrays keep
    // this allocation-free for the K = 32 model.
    std::array<u128, 78> val{};
    std::array<unsigned, 78> fv{};
    std::array<unsigned char, 77> par{};
    REQUIRE(M <= 76);
    val[0] = n;
    fv[0] = 0;
    for (unsigned j = 0; j < M; ++j) {
        par[j] = static_cast<unsigned char>(val[j] & 1u);
        val[j + 1] = collatz::T(val[j]).value;
        fv[j + 1] = fv[j] + par[j];
    }
    for (unsigned j = 0; j < M; ++j) {
        if (par[j] == 0) {
            continue;
        }
        unsigned e = j;
        while (e < M && par[e] != 0) {
            ++e;
        }
        if (e >= M || e + 1 >= M || par[e + 1] != 0) {
            continue;
        }
        if (pow2(j + 1) <= collatz::pow3(fv[j])) {
            continue;
        }
        if ((val[j] - 1) / 2 < n) {
            return true;
        }
    }
    return false;
}

} // namespace collatz::frontier_test

#endif // COLLATZ_TESTS_FRONTIER_MODEL_WALK_HPP
