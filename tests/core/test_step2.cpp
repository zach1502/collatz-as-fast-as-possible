// Step 2 candidate mask (src/core/step2.cpp): Filter::mask against the definition at
// every B in [2, 8], and the monotonicity of the mask in B.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "survivor_sample.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <vector>

using collatz::u128;
using collatz::step1::Node;
using collatz::step2::Filter;
using collatz::step2::Params;
using collatz::test::sample_survivors;

TEST_CASE("B-window mask is monotone in B (larger B => subset must-check set)") {
    // dip_B(m) is non-decreasing in B: the terms for depths k <= B_lo depend
    // only on the parity prefix (fixed by m mod 2^{B_lo}) and so are shared by
    // every larger B, which merely maximizes over additional depths. Hence a
    // candidate pruned at some B stays pruned at every larger B, i.e. the
    // "must-check" mask shrinks monotonically: mask_{B_hi} & ~mask_{B_lo} == 0.
    // (mod-9 / partial factors are identical across B, so they cancel.)
#ifdef NDEBUG
    const unsigned N = 36;
    const unsigned Bs[3] = {16u, 20u, 24u};
    const std::size_t kSample = 10000;
#else
    // Sanitizer build: shrink N/B/sample so the dip tables build fast while
    // still exercising three ascending B levels.
    const unsigned N = 24;
    const unsigned Bs[3] = {12u, 14u, 16u};
    const std::size_t kSample = 2000;
#endif
    const unsigned A = 6;
    const unsigned M = N - A;

    const std::vector<Node> sample = sample_survivors(M, kSample, 0xB16B00B5C0FFEEULL);
    REQUIRE(!sample.empty());

    const auto masks_for = [&](unsigned B) {
        Params p;
        p.N = N;
        p.A = A;
        p.B = B;
        p.num_bitvectors = 8;
        p.threads = collatz::hardware_threads();
        const Filter f(p);
        std::vector<std::uint64_t> out;
        out.reserve(sample.size());
        for (const Node& s : sample) {
            out.push_back(f.mask(s).w[0]); // A=6 => single-word mask
        }
        return out;
    };

    const std::vector<std::uint64_t> m_lo = masks_for(Bs[0]);
    const std::vector<std::uint64_t> m_mid = masks_for(Bs[1]);
    const std::vector<std::uint64_t> m_hi = masks_for(Bs[2]);

    std::uint64_t strict_shrinks = 0;
    for (std::size_t k = 0; k < sample.size(); ++k) {
        CHECK((m_mid[k] & ~m_lo[k]) == 0u); // B_mid pruned everything B_lo did
        CHECK((m_hi[k] & ~m_mid[k]) == 0u); // B_hi  pruned everything B_mid did
        if (m_hi[k] != m_lo[k]) {
            ++strict_shrinks;
        }
    }
    // The larger window must prune strictly more on some survivor,
    // otherwise the subset check is vacuous.
    CHECK(strict_shrinks > 0);
    MESSAGE("monotone masks over " << sample.size() << " survivors; strictly shrank on "
                                   << strict_shrinks);
}

// Step 2 at lookahead depth B = 2..5 (span 2^B < 64 bits). Filter accepts
// B >= 2; a build_level that sizes each bitvector as span/64 == 0 words
// makes the packing loop write bv[0] out of bounds. The sub-word read path
// also only wraps the 2^B boundary once per 64-bit window, so a correct
// packing must replicate the 2^B-periodic pattern across the whole word. The test
// compares against an independent mask from the definition (dip_B threshold and
// mod 9), with B = 6..8 as a control on the multi-word path.

namespace {

// Up to `k` Step-1 survivors of step1(M), taken in enumeration order.
std::vector<Node> first_survivors(unsigned M, std::size_t k) {
    std::vector<Node> out;
    collatz::step1::enumerate(M, collatz::step1::Options{}, [&](const Node& s) {
        if (out.size() < k) {
            out.push_back(s);
        }
    });
    return out;
}

} // namespace

TEST_CASE("Step-2 Filter mask is exact for every B in [2, 8] (incl. sub-word B < 6)") {
    const unsigned M = 16;
    const std::vector<Node> sample = first_survivors(M, 400);
    REQUIRE(!sample.empty());
    for (unsigned B = 2; B <= 8; ++B) {
        // A up to 8 makes 2^A > 2^B, i.e. the 64-bit candidate window wraps the
        // 2^B-bit vector several times -- the case a single-wrap read gets wrong.
        for (const unsigned A : {2u, 4u, 6u, 8u}) {
            Params p;
            p.N = M + A;
            p.A = A;
            p.B = B;
            p.num_bitvectors = 8;
            p.threads = 2;
            const Filter f(p);
            const std::uint64_t b_mask = (std::uint64_t{1} << B) - 1u;
            for (unsigned i = 0; i < p.num_bitvectors; ++i) {
                CHECK(f.bitvectors()[i].size() == ((std::uint64_t{1} << B) + 63u) / 64u);
            }
            std::uint64_t mismatches = 0;
            std::uint64_t kept = 0;
            for (const Node& s : sample) {
                const collatz::step2::CandidateMask got = f.mask(s);
                for (std::uint32_t a = 0; a < (1u << A); ++a) {
                    bool want;
                    const unsigned r9 = Filter::mod9(s.n0 + (u128{a} << (p.N - p.A)));
                    // NOLINTNEXTLINE(bugprone-branch-clone): one branch per exclusion rule
                    if (r9 == 2 || r9 == 4 || r9 == 5 || r9 == 8) {
                        want = false; // mod-9 exclusion.
                    } else if (s.f < f.fmin()) {
                        want = false; // 485 f <= 306 M: descends within the window.
                    } else if (s.f - f.fmin() >= p.num_bitvectors) {
                        want = true; // no lookahead bitvector for this f.
                    } else {
                        const std::uint64_t ma =
                            static_cast<std::uint64_t>(s.m + collatz::pow3(s.f) * a) & b_mask;
                        const std::int64_t t = 485LL * s.f - 306LL * M;
                        want = static_cast<std::int64_t>(Filter::dip(ma, B, nullptr, nullptr)) < t;
                    }
                    const bool have = ((got.w[a >> 6] >> (a & 63u)) & 1u) != 0u;
                    kept += have ? 1u : 0u;
                    if (have != want) {
                        ++mismatches;
                    }
                }
            }
            INFO("B=" << B << " A=" << A);
            CHECK(mismatches == 0u);
            CHECK(kept > 0u);
        }
    }
}
