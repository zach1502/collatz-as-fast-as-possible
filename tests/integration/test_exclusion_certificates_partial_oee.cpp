// The partial-OEE merge-certificate case of the exclusion certificate tests.
// test_exclusion_certificates.cpp, the other source of this executable, holds
// the sampled sieve cases and defines main.

#include "collatz/oracle.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>
#include <vector>

namespace partial_oee {

// Definitional soundness of high-bit partial OEE.
//
// For sampled Step-1 survivors n0 (mod 2^M) and every extension a >= a_star
// (a_star = Node::oee_astar, the threshold the Step-1 DFS carries and
// Filter::mask reads), the candidate n = n0 + a*2^M must carry an OEE merge
// certificate to a strictly smaller number, checked with GMP on the exact value:
//   * an arm depth j (< M) whose value m_j = T^j(n) is odd,
//   * an odd run of length l >= 1 from j, then 2 even steps, all within [0, M),
//   * (m_j - 1)/2 < n  (the merge target is strictly smaller), and
//   * Hercher's identity  T^{l+2}(m_j) == T^{l+1}((m_j - 1)/2)  (the two paths
//     coincide -- (m_j-1)/2 reaches the join one accelerated step sooner).
// It also checks the boundary: at a = a_star - 1 no completing OEE arm fires
// (a_star is minimal), so the mask keeps exactly the extensions it must.

using collatz::u128;
using collatz::step1::Node;

namespace {

mpz_class step_T(const mpz_class& x) {
    if (mpz_odd_p(x.get_mpz_t())) {
        return (3 * x + 1) / 2;
    }
    return x / 2;
}

mpz_class iterate_T(mpz_class x, unsigned k) {
    for (unsigned i = 0; i < k; ++i) {
        x = step_T(x);
    }
    return x;
}

// True when some OEE arm completing by depth M fires on the candidate n. With
// `verify_identity` it also checks that m_j is odd, half < n, and Hercher's identity.
bool oee_arm_fires(const mpz_class& n, unsigned M, bool verify_identity) {
    std::vector<mpz_class> mval(static_cast<std::size_t>(M) + 1);
    std::vector<unsigned char> par(M);
    mval[0] = n;
    for (unsigned j = 0; j < M; ++j) {
        par[j] = mpz_odd_p(mval[j].get_mpz_t()) ? 1U : 0U;
        mval[j + 1] = step_T(mval[j]);
    }
    bool fired = false;
    for (unsigned j = 0; j < M; ++j) {
        if (par[j] == 0) {
            continue;
        }
        unsigned e = j;
        while (e < M && par[e] != 0) {
            ++e; // first even at/after j; odd-run length l = e - j >= 1.
        }
        if (e >= M || (e + 1u) >= M || par[e + 1u] != 0) {
            continue; // need 2 even steps within [0, M).
        }
        const mpz_class& mj = mval[j];
        const mpz_class half = (mj - 1) / 2;
        if (!(half < n)) {
            continue; // this arm's magnitude test does not fire.
        }
        fired = true;
        if (verify_identity) {
            const unsigned l = e - j;
            CHECK(mpz_odd_p(mj.get_mpz_t()) != 0);                 // m_j odd
            CHECK(half < n);                                       // strictly smaller
            CHECK(iterate_T(mj, l + 2) == iterate_T(half, l + 1)); // Hercher L9
        }
        if (!verify_identity) {
            return true;
        }
    }
    return fired;
}

} // namespace

TEST_CASE("partial-OEE exclusions carry a genuine GMP merge certificate") {
#ifdef NDEBUG
    constexpr unsigned M = 30;
    constexpr std::size_t kSample = 4000;
#else
    constexpr unsigned M = 24;
    constexpr std::size_t kSample = 300;
#endif
    constexpr unsigned A = 6;
    constexpr std::uint32_t twoA = 1u << A;

    // A reservoir sample of the survivors mod 2^M covers the whole class range.
    std::mt19937_64 rng(0xE30ABCDEF0011223ULL);
    std::vector<Node> sample;
    sample.reserve(kSample);
    std::uint64_t seen = 0;
    collatz::step1::Options so;
    so.track_oee_astar = true;
    collatz::step1::enumerate(M, so, [&](const Node& s) {
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

    mpz_class two_M = 1;
    mpz_mul_2exp(two_M.get_mpz_t(), two_M.get_mpz_t(), M); // 2^M

    std::uint64_t classes_with_astar = 0;
    std::uint64_t excluded_verified = 0;
    for (const Node& s : sample) {
        if (s.oee_astar >= static_cast<u128>(twoA)) {
            continue; // no extension in [0, 2^A) is excluded.
        }
        const auto a_star = static_cast<std::uint32_t>(s.oee_astar);
        ++classes_with_astar;
        const mpz_class n0 = collatz::oracle::to_mpz(s.n0);

        // Every excluded extension a >= a_star must carry the certificate.
        for (std::uint32_t a = a_star; a < twoA; ++a) {
            const mpz_class n = n0 + mpz_class(static_cast<unsigned long>(a)) * two_M;
            const bool fired = oee_arm_fires(n, M, /*verify_identity=*/true);
            if (!fired) {
                CAPTURE(collatz::to_string(s.n0));
                CAPTURE(a);
                CAPTURE(a_star);
                CHECK_MESSAGE(fired, "excluded candidate lacks an OEE merge certificate");
            }
            ++excluded_verified;
        }
        // Boundary: a_star is minimal, so a_star - 1 must not fire.
        if (a_star >= 1) {
            const std::uint32_t a = a_star - 1;
            const mpz_class n = n0 + mpz_class(static_cast<unsigned long>(a)) * two_M;
            CHECK_FALSE(oee_arm_fires(n, M, /*verify_identity=*/false));
        }
    }
    MESSAGE("classes with a_star=" << classes_with_astar << " of " << sample.size()
                                   << " sampled; excluded candidates verified="
                                   << excluded_verified);
    CHECK(classes_with_astar > 0);
    CHECK(excluded_verified > 0);
}

} // namespace partial_oee
