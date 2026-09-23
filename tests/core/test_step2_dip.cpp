// Step 2 dip function (src/core/step2_dip.cpp): Filter::dip against an independent
// brute force, the parity-only mod-3 criterion, inv(3^f) mod 2^B, and the
// bitvector densities of the paper's Fig 4.1.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/collatz_fn.hpp"
#include "collatz/oracle.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"
#include "step1_gmp.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <vector>

using collatz::u128;
using collatz::step2::Filter;
using collatz::step2::Params;

namespace {

// Independent, from-the-definition brute force of dip_B (64-bit, small B).
//
// Deliberately uses the arithmetic mod-3 for the path-merge term (not the
// parity criterion). For the actual residue value m the two agree, so matching
// this against Filter::dip cross-checks both the term structure and the parity
// criterion at once.
std::uint32_t brute_dip(std::uint64_t m, unsigned B) {
    std::vector<bool> par(B);
    std::vector<unsigned> f(B + 1, 0);
    std::vector<std::uint64_t> val(B + 1);
    std::uint64_t v = m;
    val[0] = v;
    for (unsigned s = 0; s < B; ++s) {
        const bool odd = (v & 1u) != 0u;
        par[s] = odd;
        f[s + 1] = f[s] + (odd ? 1u : 0u);
        v = odd ? (3u * v + 1u) / 2u : v / 2u;
        val[s + 1] = v;
    }
    std::int64_t best = 0;
    for (unsigned k = 0; k <= B; ++k) {
        const std::int64_t k64 = static_cast<std::int64_t>(k);
        const std::int64_t f64 = static_cast<std::int64_t>(f[k]);
        const std::int64_t ak = 306 * k64 - 485 * f64;
        best = std::max(best, ak);
        // Path-merge: T^k(m) == 2 (mod 3) and at least one odd step.
        if (f[k] >= 1 && (val[k] % 3u) == 2u) {
            best = std::max(best, 306 * (k64 - 1) - 485 * (f64 - 1));
        }
        // OEE: odd run from k (l >= 1) then 2 even steps, within the window.
        if (k < B && par[k]) {
            unsigned e = k;
            while (e < B && par[e]) {
                ++e;
            }
            if (e < B && (e + 1u) < B && !par[e + 1u]) {
                best = std::max(best, 306 * (k64 + 1) - 485 * f64);
            }
        }
    }
    return best < 0 ? 0u : static_cast<std::uint32_t>(best);
}

// Fraction of set bits in bitvector i (B >= 6, so no word is replicated): the
// share of residues that survive the look-ahead at f = fmin + i.
double density(const Filter& f, unsigned i, unsigned B) {
    std::uint64_t set = 0;
    for (const std::uint64_t w : f.bitvectors()[i]) {
        set += static_cast<std::uint64_t>(std::popcount(w));
    }
    return static_cast<double>(set) / static_cast<double>(std::uint64_t{1} << B);
}

} // namespace

TEST_CASE("dip_B matches an independent brute force for small B") {
    for (const unsigned B : {8u, 10u}) {
        CAPTURE(B);
        const std::uint64_t span = std::uint64_t{1} << B;
        for (std::uint64_t m = 0; m < span; ++m) {
            const std::uint32_t got = Filter::dip(m, B, nullptr, nullptr);
            const std::uint32_t want = brute_dip(m, B);
            if (got != want) {
                CAPTURE(m);
                CHECK(got == want);
            }
        }
    }
}

// The full-span sweep above cannot reach B=31/32 (2^31/2^32 residues), but
// Filter::dip is a per-residue O(B) walk (no 2^B table), so a sampled cross-check
// covers the B>30 path cheaply. This exercises the recurrence and the stack-array
// sizing at B=31/32.
TEST_CASE("dip_B matches brute force on sampled residues at B=31/32") {
    static_assert(collatz::step2::max_B >= 32, "the B ceiling is at least 32");
    std::mt19937_64 rng(0xD1FB32E70ULL);
    for (const unsigned B : {31u, 32u}) {
        CAPTURE(B);
        const std::uint64_t span_mask =
            (B >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << B) - 1);
        // Structured edges (0, 1, 2, max residue, powers of two near the top)
        // plus a random sample. Each is an independent O(B) recomputation.
        std::vector<std::uint64_t> samples = {0u,
                                              1u,
                                              2u,
                                              3u,
                                              span_mask,
                                              span_mask - 1u,
                                              std::uint64_t{1} << (B - 1u),
                                              (std::uint64_t{1} << (B - 1u)) + 1u};
        const int kRandom = static_cast<int>(collatz::test::scaled(20000, 400));
        for (int t = 0; t < kRandom; ++t) {
            samples.push_back(rng() & span_mask);
        }
        for (const std::uint64_t m : samples) {
            const std::uint32_t got = Filter::dip(m, B, nullptr, nullptr);
            const std::uint32_t want = brute_dip(m, B);
            if (got != want) {
                CAPTURE(m);
                CHECK(got == want);
            }
        }
    }
}

TEST_CASE("parity-only mod-3 criterion equals arithmetic (Lemma 2.8)") {
    std::mt19937_64 rng(0xC011A7C0DE12ULL);
    // 100k GMP-checked trials in release; a smaller sweep under sanitizers so
    // the debug run stays quick (the parity criterion is exercised either way).
    const int kTrials = static_cast<int>(collatz::test::scaled(100000, 1500));
    int checked = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
        const std::uint64_t x = rng();
        const unsigned k = 1u + static_cast<unsigned>(rng() % 40u);
        // Build the parity bitmask via u128 stepping (avoids 64-bit overflow),
        // and compute the exact T^k(x) mod 3 via GMP.
        u128 v = x;
        std::uint64_t parity_bits = 0;
        bool overflow = false;
        for (unsigned s = 0; s < k; ++s) {
            const bool odd = (v & 1u) != 0u;
            if (odd) {
                parity_bits |= (std::uint64_t{1} << s);
            }
            const collatz::StepResult r = collatz::T(v);
            if (r.overflow) {
                overflow = true;
                break;
            }
            v = r.value;
        }
        if (overflow) {
            continue; // skip rare overflowing samples
        }
        // Criterion only defined with >= 1 odd step among the first k.
        if (parity_bits == 0) {
            continue;
        }
        ++checked;
        const mpz_class tk = collatz::test::iterate_T(collatz::oracle::to_mpz(x), k);
        const bool arithmetic = (mpz_class(tk % 3) == 2);
        const bool parity = Filter::parity_mod3_is_2(parity_bits, k);
        if (parity != arithmetic) {
            CAPTURE(x);
            CAPTURE(k);
            CHECK(parity == arithmetic);
        }
    }
    CHECK(checked > kTrials / 2);
}

TEST_CASE("inverse of 3^f mod 2^B is correct") {
    for (const unsigned B : {16u, 24u, 27u}) {
        CAPTURE(B);
        const mpz_class mod = mpz_class(1) << B;
        for (unsigned f = 0; f <= 80u; ++f) {
            CAPTURE(f);
            const std::uint64_t inv = Filter::inverse_pow3_mod2b(f, B);
            // 3^f mod 2^B via GMP, then (3^f * inv) mod 2^B must be 1.
            mpz_class p3;
            mpz_ui_pow_ui(p3.get_mpz_t(), 3u, f);
            const mpz_class a = p3 % mod;
            const mpz_class prod = (a * mpz_class(static_cast<unsigned long>(inv))) % mod;
            CHECK(prod == 1);
        }
    }
}

TEST_CASE("bitvector densities reproduce the paper's Fig 4.1") {
    // Fig 4.1 values (paper arXiv:2602.10466, N=72, A=6, B=24, i<8). These are
    // 4-significant-figure reads off a plotted figure, so a literal 1e-6 match
    // is impossible; the test asserts agreement within figure-reading precision.
    // density(f, i, B) is the exact empirical CDF of dip_B at threshold t_i, so a tight
    // match confirms the dip term set is exactly the paper's.
    const double fig[8] = {0.0199, 0.0948, 0.2288, 0.4023, 0.5847, 0.7448, 0.8630, 0.9365};

#ifdef NDEBUG
    Params p;
    p.N = 72;
    p.A = 6;
    p.B = 24;
    p.num_bitvectors = 8;
    p.threads = collatz::hardware_threads();
    const Filter f(p);
    CHECK(f.fmin() == 42u);
    constexpr double kTol = 5e-3; // figure-reading precision, not 1e-6.
    double max_abs_err = 0.0;
    for (unsigned i = 0; i < 8; ++i) {
        CAPTURE(i);
        const double d = density(f, i, p.B);
        max_abs_err = std::max(max_abs_err, std::abs(d - fig[i]));
        CHECK(std::abs(d - fig[i]) < kTol);
    }
    MESSAGE("max |density - Fig 4.1| = " << max_abs_err << " (tol " << kTol << ")");
#else
    // Sanitizer build: building a 2^24 dip table is slow, so only sanity-check
    // that densities form a monotone CDF on a small B and stay within [0,1].
    (void) fig;
    Params p;
    p.N = 72;
    p.A = 6;
    p.B = 16;
    p.num_bitvectors = 8;
    p.threads = collatz::hardware_threads();
    const Filter f(p);
    double prev = -1.0;
    for (unsigned i = 0; i < 8; ++i) {
        const double d = density(f, i, p.B);
        CHECK(d >= 0.0);
        CHECK(d <= 1.0);
        CHECK(d >= prev); // threshold t_i grows, so the CDF is non-decreasing.
        prev = d;
    }
#endif
}
