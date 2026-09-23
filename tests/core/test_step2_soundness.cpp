// Step 2 soundness (Theorem 4.4): every candidate the look-ahead bitvectors exclude
// descends or merges within M+B steps, checked in GMP, and the fast soundness
// witness against its definitional form.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"
#include "soundness_witness.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <string>
#include <vector>

using collatz::u128;
using collatz::step1::Node;
using collatz::step2::Filter;
using collatz::step2::Params;

namespace {

mpz_class step_T(const mpz_class& x) {
    if (mpz_odd_p(x.get_mpz_t())) {
        return (3 * x + 1) / 2;
    }
    return x / 2;
}

// Theorem 4.4 soundness witness, definitional form: does n = n0 + a*2^M
// descend below itself, or merge with a strictly smaller number, within M+B
// steps? Checks descent, path-merge (v == 2 mod 3, (2v-1)/3 < n), and OEE (odd v
// with (v-1)/2 < n followed by >= 1 odd steps then 2 even steps), all within the
// window. The soundness sweeps call the faster, allocation-free
// collatz::test::descends_or_merges (soundness_witness.hpp); this definitional
// copy is its reference, and the next test case pins the two equal.
bool descends_or_merges_ref(const mpz_class& n, unsigned steps) {
    std::vector<mpz_class> val(steps + 1);
    std::vector<bool> par(steps);
    val[0] = n;
    mpz_class v = n;
    for (unsigned s = 0; s < steps; ++s) {
        par[s] = mpz_odd_p(v.get_mpz_t()) != 0;
        v = step_T(v);
        val[s + 1] = v;
    }
    for (unsigned s = 1; s <= steps; ++s) {
        if (val[s] < n) {
            return true;
        }
    }
    for (unsigned s = 0; s <= steps; ++s) {
        if (mpz_class(val[s] % 3) == 2) {
            const mpz_class pred = (2 * val[s] - 1) / 3;
            if (pred < n) {
                return true;
            }
        }
    }
    for (unsigned j = 0; j < steps; ++j) {
        if (!par[j]) {
            continue;
        }
        const mpz_class pred = (val[j] - 1) / 2;
        if (!(pred < n)) {
            continue;
        }
        unsigned e = j;
        while (e < steps && par[e]) {
            ++e;
        }
        if (e < steps && (e + 1u) < steps && !par[e + 1u]) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("fast soundness witness == definitional descends_or_merges") {
    // The early-exit, buffer-reusing witness must return the same boolean as the
    // literal definition for every (n, steps). Mix random wide n (both outcomes
    // occur at short windows) with the window lengths the sweeps use.
    std::mt19937_64 rng(0x5EEDF457ULL);
    const int kTrials = static_cast<int>(collatz::test::scaled(20000, 2000));
    std::uint64_t trues = 0;
    std::uint64_t falses = 0;
    for (int i = 0; i < kTrials; ++i) {
        const unsigned bits = 8u + static_cast<unsigned>(rng() % 57u); // 8..64-bit n
        std::uint64_t x = rng() >> (64u - bits);
        x |= std::uint64_t{1} << (bits - 1u);
        const mpz_class n(std::to_string(x));
        for (const unsigned steps : {1u, 2u, 3u, 8u, 20u, 46u, 60u}) {
            const bool want = descends_or_merges_ref(n, steps);
            const bool got = collatz::test::descends_or_merges(n, steps);
            if (got != want) {
                CAPTURE(x);
                CAPTURE(steps);
                CHECK(got == want);
            }
            (want ? trues : falses) += 1u;
        }
    }
    CHECK(trues > 0); // both outcomes are exercised
    CHECK(falses > 0);
}

TEST_CASE("Theorem 4.4 soundness: bv-excluded candidates descend or merge (GMP)") {
    // Full config in release. Under sanitizers N, the sample count and the
    // look-ahead B shrink: the 2^B dip table dominates the Filter build, and B=24
    // (16M entries) is too slow under ASan. Theorem 4.4 holds for any B.
    const unsigned N = collatz::test::scaled(36, 24);
    const std::size_t kSample = collatz::test::scaled(8000, 400);
    const unsigned B = collatz::test::scaled(24, 18);
    Params p;
    p.N = N;
    p.A = 6;
    p.B = B;
    p.num_bitvectors = 8;
    p.threads = collatz::hardware_threads();
    const Filter filter(p);
    const unsigned M = N - p.A;
    const unsigned window = M + p.B;
    const std::uint64_t b_mask = (std::uint64_t{1} << p.B) - 1;

    // Reservoir-sample survivors of step1(M) to cover the whole class range.
    std::mt19937_64 rng(0x5EED1234ABCD0055ULL);
    std::vector<Node> sample;
    sample.reserve(kSample);
    std::uint64_t seen = 0;
    collatz::step1::enumerate(M, collatz::step1::Options{}, [&](const Node& s) {
        ++seen;
        if (sample.size() < kSample) {
            sample.push_back(s);
        } else {
            const std::uint64_t r = rng() % seen;
            if (r < kSample) {
                sample[static_cast<std::size_t>(r)] = s;
            }
        }
    });
    REQUIRE(!sample.empty());

    std::uint64_t bv_excluded_checked = 0;
    std::uint64_t below_fmin_checked = 0;
    std::uint64_t below_fmin_late = 0; // f<fmin drops but only after window steps.
    for (const Node& s : sample) {
        const mpz_class n0 = collatz::oracle::to_mpz(s.n0);
        for (std::uint32_t a = 0; a < (1u << p.A); ++a) {
            const mpz_class n = n0 + (mpz_class(static_cast<unsigned long>(a)) << M);
            // mod-9 exclusions have a separate justification; skip them here.
            const unsigned r = Filter::mod9(s.n0 + (u128{a} << M));
            const bool mod9_excluded = (r == 2 || r == 4 || r == 5 || r == 8);
            if (mod9_excluded) {
                continue;
            }
            if (s.f < filter.fmin()) {
                // f < fmin guarantees an eventual descent, which can come after
                // the window; the test checks the descent and counts late ones.
                ++below_fmin_checked;
                if (!collatz::test::descends_or_merges(n, window)) {
                    ++below_fmin_late;
                    // n < 2^N is in the verified range, so it descends within
                    // the 4000-step cap.
                    CHECK(collatz::test::descends_or_merges(n, 4000u));
                }
                continue;
            }
            const unsigned i = s.f - filter.fmin();
            if (i >= p.num_bitvectors) {
                continue; // no lookahead available for this f.
            }
            const std::uint64_t ma =
                static_cast<std::uint64_t>((s.m + collatz::pow3(s.f) * a) & b_mask);
            const std::int64_t t = 485LL * s.f - 306LL * M;
            const bool bv_excluded =
                static_cast<std::int64_t>(Filter::dip(ma, p.B, nullptr, nullptr)) >= t;
            if (!bv_excluded) {
                continue;
            }
            ++bv_excluded_checked;
            if (!collatz::test::descends_or_merges(n, window)) {
                CAPTURE(collatz::to_string(s.n0));
                CAPTURE(a);
                CAPTURE(s.f);
                CHECK_MESSAGE(false, "bv-excluded candidate neither descends nor merges");
            }
        }
    }
    CHECK(bv_excluded_checked > 0);
    MESSAGE("bv-excluded candidates verified: " << bv_excluded_checked);
    MESSAGE("f<fmin candidates verified: " << below_fmin_checked << " (dropped only after window: "
                                           << below_fmin_late << ")");
}
