// Step 1 (src/core/step1.cpp): the class sieve against GMP.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "step1_gmp.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>
#include <vector>

using collatz::u128;
using collatz::step1::Node;
using collatz::step1::Options;
using collatz::step1::Reason;
using collatz::test::Excl;
using collatz::test::is_odd;
using collatz::test::iterate_T;
using collatz::test::step_T;

namespace {

// Trajectory values m_0..m_k (m_i = T^i(start)) for a class representative.
std::vector<mpz_class> trajectory(u128 start, unsigned k) {
    std::vector<mpz_class> v;
    v.reserve(k + 1);
    mpz_class x = collatz::oracle::to_mpz(start);
    v.push_back(x);
    for (unsigned i = 0; i < k; ++i) {
        x = step_T(x);
        v.push_back(x);
    }
    return v;
}

// Number of odd steps among the first M steps of n0's trajectory.
unsigned odd_count(u128 n0, unsigned M) {
    unsigned f = 0;
    mpz_class x = collatz::oracle::to_mpz(n0);
    for (unsigned i = 0; i < M; ++i) {
        if (is_odd(x)) {
            ++f;
        }
        x = step_T(x);
    }
    return f;
}

} // namespace

TEST_CASE("step1 sieve is correct against a GMP brute force") {
    for (const unsigned M : {4u, 5u, 6u, 8u, 10u}) {
        CAPTURE(M);

        std::vector<Excl> excl;
        Options opts;
        opts.on_exclude = [&excl](u128 n0, unsigned depth, int reason) {
            excl.push_back(Excl{n0, depth, reason});
        };

        std::vector<Node> survivors;
        collatz::step1::enumerate(M, opts, [&survivors](const Node& n) { survivors.push_back(n); });

        const u128 span = static_cast<u128>(1) << M;

        // Partition: every n in [0, 2^M) is either convention-excluded
        // (even or 1 mod 4), a survivor, or covered by exactly one sieved
        // class -- and these are mutually exclusive.
        for (u128 n = 0; n < span; ++n) {
            int fates = 0;
            const bool convention = ((n & 1) == 0) || ((n & 3) == 1);
            if (convention) {
                ++fates;
            }
            bool is_survivor = false;
            for (const Node& s : survivors) {
                if (s.n0 == n) {
                    is_survivor = true;
                    break;
                }
            }
            if (is_survivor) {
                ++fates;
            }
            int covering = 0;
            for (const Excl& e : excl) {
                const u128 modulus = static_cast<u128>(1) << e.depth;
                if ((n & (modulus - 1)) == e.n0) {
                    ++covering;
                }
            }
            fates += covering;
            // Convention classes are all even or 1 mod 4; sieve/survivor classes
            // are all 3 mod 4, so they never overlap. Exactly one fate total.
            CHECK_MESSAGE(fates == 1, "n=" << collatz::to_string(n) << " fates=" << fates);
        }

        // Every exclusion has a valid certificate.
        for (const Excl& e : excl) {
            CAPTURE(collatz::to_string(e.n0));
            CAPTURE(e.depth);
            const std::vector<mpz_class> traj = trajectory(e.n0, e.depth);
            const mpz_class& m = traj[e.depth]; // T^k(n0)
            const mpz_class n0 = collatz::oracle::to_mpz(e.n0);

            if (e.reason == Reason::reason_descent) {
                // Descent: T^k(n0) < n0.
                CHECK(m < n0);
            } else if (e.reason == Reason::reason_path_merge) {
                // Path-merge: m == 2 (mod 3), (2m-1)/3 < n0, T((2m-1)/3)==m.
                // The modulus is a concrete mpz_class: doctest would otherwise
                // stringify the GMP expression template via libgmpxx's ostream
                // operator, which the tests do not link (see collatz/oracle.hpp).
                CHECK(mpz_class(m % 3) == 2);
                const mpz_class pred = (2 * m - 1) / 3;
                CHECK(pred < n0);
                CHECK(step_T(pred) == m);
            } else {
                REQUIRE(e.reason == Reason::reason_oee);
                // OEE: locate the maximal odd run ending at depth k-3, then
                // verify the merge and Hercher identity.
                const unsigned k = e.depth;
                REQUIRE(k >= 4); // need j < k-2 with l >= 1 odd step
                // Steps k-2 and k-1 (parities of m_{k-2}, m_{k-1}) must be even.
                CHECK_FALSE(is_odd(traj[k - 2]));
                CHECK_FALSE(is_odd(traj[k - 1]));
                // Walk back over odd steps to find the run start j.
                unsigned j = k - 2;
                while (j > 0 && is_odd(traj[j - 1])) {
                    --j;
                }
                REQUIRE(j <= k - 3); // at least one odd step: l >= 1
                const mpz_class& mj = traj[j];
                CHECK(is_odd(mj));
                const mpz_class pred = (mj - 1) / 2;
                CHECK(pred < n0);
                const unsigned l = k - 2 - j;
                // Hercher: C^{l+2}(m_j) == C^{l+1}((m_j-1)/2).
                CHECK(iterate_T(mj, l + 2) == iterate_T(pred, l + 1));
            }
        }

        // Survivors: m == GMP T^M(n0) and f == odd-step count.
        for (const Node& s : survivors) {
            CAPTURE(collatz::to_string(s.n0));
            CHECK(collatz::oracle::to_mpz(s.m) == iterate_T(collatz::oracle::to_mpz(s.n0), M));
            CHECK(s.f == odd_count(s.n0, M));
            CHECK((s.n0 & 3) == 3); // every survivor is 3 mod 4
        }
    }
}
