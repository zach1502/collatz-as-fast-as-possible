#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <vector>

// Certificate invariant: for a 1-in-K sample of the exclusions of every sieve,
// the merge or descent certificate target, recomputed with GMP, is strictly less
// than n. This catches a self-merge such as a partial-OEE prune at D == 0, whose
// target equals n. Step 1 is sampled through its on_exclude callback, Step 2
// through the partial-OEE threshold a* that the Step-1 DFS carries.
//
// The sample is 1 in 64, chosen by a running counter: the GMP recompute stays
// cheap and still covers thousands of exclusions per sieve.

using collatz::u128;
using collatz::step1::Node;

namespace {

mpz_class step_T(const mpz_class& m) {
    if (mpz_odd_p(m.get_mpz_t()) != 0) {
        return (3 * m + 1) / 2;
    }
    return m / 2;
}

mpz_class iterate_T(mpz_class m, unsigned k) {
    for (unsigned i = 0; i < k; ++i) {
        m = step_T(m);
    }
    return m;
}

} // namespace

TEST_CASE("Step-1 exclusions carry a strict GMP certificate target < n0 (MAJOR-1)") {
    const unsigned M = collatz::test::scaled(24, 18);
    constexpr int kSample = 64;

    collatz::step1::Options opt;
    opt.mod3t = 6; // the default sieve.

    collatz::ref::Options ro;
    ro.descent = true;
    ro.path_merge = true;
    ro.oee = true;
    ro.mod3t = 6;

    std::uint64_t seen = 0;
    std::uint64_t checked = 0;
    std::uint64_t counter = 0;

    opt.on_exclude = [&](u128 n0_u, unsigned depth, int reason) {
        ++seen;
        if ((counter++ % kSample) != 0) {
            return;
        }
        ++checked;
        const mpz_class n0 = collatz::oracle::to_mpz(n0_u);
        const mpz_class m = iterate_T(n0, depth); // m = T^depth(n0).

        switch (reason) {
        case collatz::step1::reason_descent: {
            CHECK(m < n0); // target = T^depth(n0).
            break;
        }
        case collatz::step1::reason_path_merge: {
            const mpz_class m_mod3 = m % 3;
            CHECK(m_mod3 == 2);
            const mpz_class target = (2 * m - 1) / 3;
            CHECK(target < n0);
            break;
        }
        case collatz::step1::reason_oee: {
            unsigned j = 0;
            unsigned k = 0;
            REQUIRE(collatz::ref::oee_witness(n0_u, M, ro, j, k));
            const mpz_class mj = iterate_T(n0, j);
            CHECK(mpz_odd_p(mj.get_mpz_t()) != 0);
            const mpz_class target = (mj - 1) / 2;
            CHECK(target < n0); // strict: a target == n0 is a self-merge.
            break;
        }
        case collatz::step1::reason_mod3t: {
            // The mod-3^t chain target has no closed form here, so the check is
            // that the independent GMP reference also excludes this class.
            const collatz::ref::Classification c = collatz::ref::classify(n0_u, M, ro);
            CHECK(c.by != collatz::ref::Excl::none);
            break;
        }
        default:
            FAIL("unknown exclusion reason ", reason);
        }
    };

    const collatz::step1::Stats st = collatz::step1::enumerate(M, opt, [](const Node&) {});
    CHECK(seen > 0);
    CHECK(checked > 0);
    // The sieve does not exclude every class.
    CHECK(st.survivors > 0);
}

TEST_CASE("Step-2 partial-OEE exclusions carry a strict GMP target < n (MAJOR-1)") {
    const unsigned M = collatz::test::scaled(24, 18);
    constexpr unsigned A = 6;
    constexpr int kSample = 64;
    const u128 twoM = static_cast<u128>(1) << M;

    // The partial-OEE threshold a* that Filter::mask reads (Node::oee_astar).
    collatz::step1::Options so;
    so.track_oee_astar = true;
    std::vector<Node> survs;
    collatz::step1::enumerate(M, so, [&survs](const Node& n) { survs.push_back(n); });

    std::uint64_t checked = 0;
    std::uint64_t counter = 0;
    for (const Node& s : survs) {
        if (s.oee_astar >= (static_cast<u128>(1) << A)) {
            continue; // nothing excluded for this survivor.
        }
        const auto a_star = static_cast<std::uint32_t>(s.oee_astar);
        if ((counter++ % kSample) != 0) {
            continue; // sample 1-in-K survivors that do exclude something.
        }
        // Every excluded extension a in [a_star, 2^A) (a_star may be 0 when a
        // D<0 arm merges the whole class below n0) must have an OEE target
        // strictly < n = n0 + a*2^M.
        for (std::uint32_t a = a_star; a < (1u << A); ++a) {
            ++checked;
            const mpz_class n =
                collatz::oracle::to_mpz(s.n0) + mpz_class(a) * collatz::oracle::to_mpz(twoM);
            // The M-step prefix, walked in GMP, must give an OEE target strictly
            // below n among arms completing by depth M.
            bool ok = false;
            std::vector<mpz_class> mval(static_cast<std::size_t>(M) + 1);
            std::vector<unsigned char> par(M);
            mval[0] = n;
            for (unsigned j = 0; j < M; ++j) {
                par[j] = mpz_odd_p(mval[j].get_mpz_t()) != 0 ? 1U : 0U;
                mval[j + 1] = step_T(mval[j]);
            }
            for (unsigned j = 0; j + 2 < M && !ok; ++j) {
                if (par[j] == 0) {
                    continue;
                }
                unsigned e = j;
                while (e < M && par[e] != 0) {
                    ++e;
                }
                if (e < M && (e + 1) < M && par[e] == 0 && par[e + 1] == 0) {
                    if ((mval[j] - 1) / 2 < n) {
                        ok = true; // OEE target strictly below n.
                    }
                }
            }
            CAPTURE(collatz::to_string(s.n0));
            CAPTURE(a);
            CHECK(ok); // an excluded a with no target < n would be unsound.
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("the strict guard rejects a target == n (BLOCKER-1 would fire)") {
    // Real near-boundary value: n0=83, T^26(83) = 167 = 2*83+1, target = 83.
    const mpz_class n0 = 83;
    const mpz_class m = iterate_T(n0, 26);
    const mpz_class target = (m - 1) / 2;
    CHECK(target == n0);
    // A non-strict a* test excludes a=0 here; the strict certificate check refuses it.
    CHECK_FALSE(target < n0);
}
