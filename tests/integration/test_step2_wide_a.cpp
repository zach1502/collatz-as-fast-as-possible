#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "scale.hpp"
#include "soundness_witness.hpp"
#include "test_env.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <thread>
#include <vector>

// Wide-A coverage for step2::Filter at A = 8, 12 and 14, where the candidate
// mask spans several words. The tests check:
//   (1) the mask bit of every extension a equals the predicate
//       dip_B window keep && mod-9 keep && a < a*, recomputed from the static
//       primitives, with a* the partial-OEE threshold the Step-1 DFS carries;
//   (2) Theorem 4.4 soundness: every bitvector-excluded candidate descends or
//       merges within M+B steps (GMP).

using collatz::u128;
using collatz::step1::Node;
using collatz::step2::CandidateMask;
using collatz::step2::Filter;
using collatz::step2::Params;

namespace {

// The low B bits (b_mask = 2^B - 1) of extension a's value after the M-step
// prefix, m_a = m + 3^f a. f <= M <= 28 and a < 2^14 keep m_a below 2^64.
std::uint64_t candidate_m_low(const Node& s, std::uint32_t a, std::uint64_t b_mask) {
    return static_cast<std::uint64_t>((s.m + collatz::pow3(s.f) * a) & b_mask);
}

// Reservoir-sample up to `k` Step-1 survivors of step1(M) so the samples cover
// the whole class range without materializing every survivor.
std::vector<Node> sample_survivors(unsigned M, std::size_t k, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Node> sample;
    sample.reserve(k);
    std::uint64_t seen = 0;
    collatz::step1::Options so;
    so.track_oee_astar = true; // Filter::mask reads Node::oee_astar.
    collatz::step1::enumerate(M, so, [&](const Node& s) {
        ++seen;
        if (sample.size() < k) {
            sample.push_back(s);
        } else {
            const std::uint64_t r = rng() % seen;
            if (r < k) {
                sample[static_cast<std::size_t>(r)] = s;
            }
        }
    });
    return sample;
}

// Worker threads for the GMP soundness sweeps: up to 4 (the PROCESSORS hint
// tests/register_integration_cpu_a.cmake gives these entries), never more than
// the hardware or the number of survivors.
unsigned soundness_workers(std::size_t n) {
    unsigned w = std::min(4u, std::max(1u, collatz::hardware_threads()));
    if (n < w) {
        w = n == 0 ? 1u : static_cast<unsigned>(n);
    }
    return w;
}

} // namespace

namespace {

// Definitional check: for a filter at (N, A, B, NBV) with partial-OEE on, every
// extension a's mask bit equals the definitional predicate
//   dip_B(m_a mod 2^B) < t_i  &&  mod-9 keep  &&  a < a*
// recomputed from the static primitives, with a* the survivor's carried
// Node::oee_astar. Exercises the multi-word window read, the multi-word mod-9
// masks and the multi-word partial-OEE a* prefix at once.
void check_definitional(unsigned A, std::size_t kSample, std::uint64_t seed) {
    const unsigned N = collatz::test::scaled(36, 26);
    const unsigned B = collatz::test::scaled(24, 16);
    const unsigned NBV = 8;
    const unsigned M = N - A;

    Params p;
    p.N = N;
    p.A = A;
    p.B = B;
    p.num_bitvectors = NBV;
    p.threads = collatz::hardware_threads();
    // ctest-scoped cache only (see scoped_bv_cache()); a hit is bit-identical to
    // the fresh build, and the checks below recompute dip from Filter::dip.
    p.use_bv_cache = collatz::test::scoped_bv_cache();
    p.partial_oee = true;
    const Filter filter(p);

    const std::uint64_t b_mask = (std::uint64_t{1} << B) - 1;
    const std::uint32_t twoA = 1u << A;

    const std::vector<Node> sample = sample_survivors(M, kSample, seed);
    REQUIRE(!sample.empty());

    std::uint64_t set_bits = 0;
    for (const Node& s : sample) {
        const CandidateMask mask = filter.mask(s);
        REQUIRE(mask.words == (twoA + 63u) / 64u);

        for (std::uint32_t a = 0; a < twoA; ++a) {
            // mod-9 keep: (n0 + a*2^M) mod 9 not in {2,4,5,8}.
            const u128 n = s.n0 + (static_cast<u128>(a) << M);
            const unsigned r = Filter::mod9(n);
            const bool mod9_ok = !(r == 2 || r == 4 || r == 5 || r == 8);

            // bitvector-window keep: dip_B(m_a mod 2^B) < t_i, with the same
            // f<fmin / i>=NBV fast paths the filter uses.
            bool bv_ok = false;
            if (s.f >= filter.fmin()) {
                const unsigned i = s.f - filter.fmin();
                if (i >= NBV) {
                    bv_ok = true; // no lookahead available => not pruned here
                } else {
                    const std::uint64_t ma = candidate_m_low(s, a, b_mask);
                    const std::int64_t t = 485LL * static_cast<std::int64_t>(s.f) -
                                           306LL * static_cast<std::int64_t>(M);
                    bv_ok = static_cast<std::int64_t>(Filter::dip(ma, B, nullptr, nullptr)) < t;
                }
            }

            // partial-OEE keep: a < a*, the threshold the Step-1 DFS carries
            // (Node::oee_astar; a value of 2^A or more keeps every a).
            const bool partial_ok = static_cast<u128>(a) < s.oee_astar;
            const bool expected = bv_ok && mod9_ok && partial_ok;

            if (mask.test(a) != expected) {
                CAPTURE(A);
                CAPTURE(collatz::to_string(s.n0));
                CAPTURE(a);
                CAPTURE(s.f);
                CAPTURE(bv_ok);
                CAPTURE(mod9_ok);
                CAPTURE(partial_ok);
                CHECK(mask.test(a) == expected);
            }
            set_bits += expected ? 1u : 0u;
        }
    }
    CHECK(set_bits > 0); // the predicate is non-vacuous
    MESSAGE("A=" << A << " wide-mask bits verified over " << sample.size()
                 << " survivors; set bits = " << set_bits);
}

// Theorem 4.4 soundness: every bitvector-excluded candidate descends or merges
// within M+B steps (GMP).
void check_soundness(unsigned A, std::size_t kSample, std::uint64_t seed) {
    const unsigned N = collatz::test::scaled(36, 26);
    const unsigned B = collatz::test::scaled(24, 16);
    const unsigned NBV = 8;
    const unsigned M = N - A;
    const unsigned window = M + B;
    const std::uint64_t b_mask = (std::uint64_t{1} << B) - 1;

    Params p;
    p.N = N;
    p.A = A;
    p.B = B;
    p.num_bitvectors = NBV;
    p.threads = collatz::hardware_threads();
    // ctest-scoped cache only (see scoped_bv_cache()); a hit is bit-identical to
    // the fresh build, and the checks below recompute dip from Filter::dip.
    p.use_bv_cache = collatz::test::scoped_bv_cache();
    const Filter filter(p);

    const std::vector<Node> sample = sample_survivors(M, kSample, seed);
    REQUIRE(!sample.empty());

    // The GMP walks are independent per survivor, so the sample is split into
    // contiguous slices over kWorkers threads. doctest assertions are not
    // thread-safe, so workers only count and record, and every failure is
    // reported below on the main thread with its CAPTUREs. The candidate set
    // checked, and so bv_excluded_checked, is identical to a serial pass.
    struct Failure {
        u128 n0;
        std::uint32_t a;
        unsigned f;
    };
    const unsigned kWorkers = soundness_workers(sample.size());
    std::vector<std::uint64_t> checked(kWorkers, 0);
    std::vector<std::vector<Failure>> failures(kWorkers);
    const auto work = [&](unsigned w) {
        const std::size_t lo = sample.size() * w / kWorkers;
        const std::size_t hi = sample.size() * (w + 1u) / kWorkers;
        mpz_class n;
        for (std::size_t idx = lo; idx < hi; ++idx) {
            const Node& s = sample[idx];
            if (s.f < filter.fmin()) {
                continue; // f<fmin justification is separate (see test_core_step2_soundness)
            }
            const unsigned i = s.f - filter.fmin();
            if (i >= NBV) {
                continue; // no lookahead available for this f
            }
            const mpz_class n0 = collatz::oracle::to_mpz(s.n0);
            const std::int64_t t =
                485LL * static_cast<std::int64_t>(s.f) - 306LL * static_cast<std::int64_t>(M);
            for (std::uint32_t a = 0; a < (1u << A); ++a) {
                // mod-9 exclusions have a separate justification; skip them here.
                const unsigned r = Filter::mod9(s.n0 + (static_cast<u128>(a) << M));
                if (r == 2 || r == 4 || r == 5 || r == 8) {
                    continue;
                }
                const std::uint64_t ma = candidate_m_low(s, a, b_mask);
                const bool bv_excluded =
                    static_cast<std::int64_t>(Filter::dip(ma, B, nullptr, nullptr)) >= t;
                if (!bv_excluded) {
                    continue;
                }
                ++checked[w];
                // n = n0 + a*2^M, formed only for the walked candidates.
                mpz_mul_2exp(
                    n.get_mpz_t(), mpz_class(static_cast<unsigned long>(a)).get_mpz_t(), M);
                n += n0;
                if (!collatz::test::descends_or_merges(n, window)) {
                    failures[w].push_back(Failure{s.n0, a, s.f});
                }
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned w = 1; w < kWorkers; ++w) {
        pool.emplace_back(work, w);
    }
    work(0);
    for (std::thread& th : pool) {
        th.join();
    }

    std::uint64_t bv_excluded_checked = 0;
    for (unsigned w = 0; w < kWorkers; ++w) {
        bv_excluded_checked += checked[w];
        for (const Failure& fl : failures[w]) {
            CAPTURE(A);
            CAPTURE(collatz::to_string(fl.n0));
            CAPTURE(fl.a);
            CAPTURE(fl.f);
            CHECK_MESSAGE(false, "bv-excluded candidate neither descends nor merges");
        }
    }
    CHECK(bv_excluded_checked > 0);
    MESSAGE("A=" << A << " bv-excluded candidates verified: " << bv_excluded_checked);
}

} // namespace

TEST_CASE("wide-A mask (A=8) equals the definitional predicate bit-for-bit") {
    check_definitional(8, collatz::test::scaled(10000, 400), 0xE34A8ULL);
}

// A above 10, up to max_A = 14. A mask has 64 words at A=12 and 256 words
// (2 KB) at A=14, and each survivor costs 2^A predicate evaluations, so the
// sample holds 10^3 survivors. These are the widest multi-word window reads,
// mod-9 masks and a* prefixes.
TEST_CASE("wide-A mask (A=12) equals the definitional predicate bit-for-bit") {
    check_definitional(12, collatz::test::scaled(1000, 60), 0xE53A12ULL);
}

TEST_CASE("wide-A mask (A=14) equals the definitional predicate bit-for-bit") {
    check_definitional(14, collatz::test::scaled(1000, 40), 0xE53A14ULL);
}

TEST_CASE("Theorem 4.4 soundness at A=8: bv-excluded candidates descend or merge (GMP)") {
    check_soundness(8, collatz::test::scaled(6000, 300), 0x5011DA8ULL);
}

TEST_CASE("Theorem 4.4 soundness at A=12: bv-excluded candidates descend or merge (GMP)") {
    check_soundness(12, collatz::test::scaled(1000, 60), 0x5011DA12ULL);
}

TEST_CASE("Theorem 4.4 soundness at A=14: bv-excluded candidates descend or merge (GMP)") {
    check_soundness(14, collatz::test::scaled(1000, 40), 0x5011DA14ULL);
}
