// Step 1 (src/core/step1_sieve.hpp): the mod-3^t exclusions against an
// independent GMP chain search, and a prebuilt mod-3^t chain table.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "step1_gmp.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <utility>
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

// Minimal-ratio backward preimage chain of the concrete m (depth <= 2t, <= t
// odd steps), computed independently in GMP.
struct Chain {
    bool found = false;
    unsigned L = 0;
    unsigned v = 0;
    mpz_class A;
};

bool better(unsigned l1, unsigned v1, unsigned l2, unsigned v2) {
    mpz_class lhs, rhs, p;
    mpz_ui_pow_ui(p.get_mpz_t(), 3, v2);
    lhs = p;
    mpz_mul_2exp(lhs.get_mpz_t(), lhs.get_mpz_t(), l1);
    mpz_ui_pow_ui(p.get_mpz_t(), 3, v1);
    rhs = p;
    mpz_mul_2exp(rhs.get_mpz_t(), rhs.get_mpz_t(), l2);
    return lhs < rhs;
}

void brute(
    const mpz_class& value, unsigned L, unsigned v, unsigned t, unsigned depth_left, Chain& best) {
    if (v >= 1) {
        mpz_class num = 1, den;
        mpz_mul_2exp(num.get_mpz_t(), num.get_mpz_t(), L); // 2^L
        mpz_ui_pow_ui(den.get_mpz_t(), 3, v);              // 3^v
        if (num < den) {
            const bool take = !best.found || better(L, v, best.L, best.v) ||
                              (L == best.L && v == best.v && value < best.A);
            if (take) {
                best.found = true;
                best.L = L;
                best.v = v;
                best.A = value;
            }
        }
    }
    if (depth_left == 0) {
        return;
    }
    brute(2 * value, L + 1, v, t, depth_left - 1, best);
    if (v < t && mpz_class(value % 3) == 2) {
        brute((2 * value - 1) / 3, L + 1, v + 1, t, depth_left - 1, best);
    }
}

unsigned odd_steps(const mpz_class& n0, unsigned k) {
    mpz_class x = n0;
    unsigned f = 0;
    for (unsigned i = 0; i < k; ++i) {
        if (is_odd(x)) {
            ++f;
        }
        x = step_T(x);
    }
    return f;
}

} // namespace

TEST_CASE("every mod3t exclusion is a sound class-wide merge to a smaller number") {
    for (const unsigned t : {2u, 3u, 4u}) {
        bool saw_mod3t = false;
        for (const unsigned M : {12u, 14u, 16u, 18u}) {
            CAPTURE(t);
            CAPTURE(M);

            std::vector<Excl> excl;
            Options opts;
            opts.mod3t = t;
            opts.on_exclude = [&excl](u128 n0, unsigned depth, int reason) {
                excl.push_back(Excl{n0, depth, reason});
            };
            collatz::step1::enumerate(M, opts, [](const Node&) {});

            for (const Excl& e : excl) {
                if (e.reason != Reason::reason_mod3t) {
                    continue;
                }
                saw_mod3t = true;
                CAPTURE(collatz::to_string(e.n0));
                CAPTURE(e.depth);
                const unsigned k = e.depth;
                const mpz_class n0 = collatz::oracle::to_mpz(e.n0);
                const mpz_class m = iterate_T(n0, k); // T^k(n0)

                // Independent chain search must find a value-decreasing chain.
                Chain c;
                brute(m, 0, 0, t, 2 * t, c);
                REQUIRE(c.found);
                CHECK(c.v >= 1);
                CHECK(c.v <= t);

                // (integrality and preimage) A is an integer with T^L(A)=m.
                CHECK(iterate_T(c.A, c.L) == m);
                // (ii) base: strictly smaller than the class representative.
                CHECK(c.A < n0);
                CHECK(c.A >= 1);

                const unsigned f = odd_steps(n0, k);
                // (i) residue uniformity requirement.
                CHECK(f >= c.v);
                // (iii) slope: 2^L * 3^{f-v} <= 2^k.
                mpz_class slope = 1, p3, bound = 1;
                mpz_mul_2exp(slope.get_mpz_t(), slope.get_mpz_t(), c.L);
                mpz_ui_pow_ui(p3.get_mpz_t(), 3, f - c.v);
                slope *= p3;
                mpz_mul_2exp(bound.get_mpz_t(), bound.get_mpz_t(), k);
                CHECK(slope <= bound);

                // K recovered from the base point; A_a for extensions must be a
                // smaller preimage of T^k(n) for the whole class.
                mpz_class twoL = 1;
                mpz_mul_2exp(twoL.get_mpz_t(), twoL.get_mpz_t(), c.L);
                mpz_class den;
                mpz_ui_pow_ui(den.get_mpz_t(), 3, c.v);
                const mpz_class K = twoL * m - den * c.A; // exact
                mpz_class p3f;
                mpz_ui_pow_ui(p3f.get_mpz_t(), 3, f); // 3^f
                mpz_class twok = 1;
                mpz_mul_2exp(twok.get_mpz_t(), twok.get_mpz_t(), k); // 2^k

                for (unsigned a = 1; a <= 20; ++a) {
                    const mpz_class n_a = n0 + a * twok;
                    const mpz_class m_a = m + a * p3f; // affine lemma
                    CHECK(iterate_T(n_a, k) == m_a);   // f is class-invariant
                    const mpz_class numer = twoL * m_a - K;
                    CHECK(mpz_class(numer % den) == 0); // integral for all a
                    const mpz_class A_a = numer / den;
                    CHECK(iterate_T(A_a, c.L) == m_a); // a preimage
                    CHECK(A_a < n_a);                  // strictly smaller
                }
            }
        }
        // Each t exercises the sieve somewhere in this M range.
        CHECK(saw_mod3t);
    }
}

TEST_CASE("a prebuilt mod-3^t table gives the same enumeration; one for another t is ignored") {
    constexpr unsigned M = 22;
    collatz::step1::Options o;
    o.mod3t = 6;
    // The survivors and the number of mod-3^t exclusions.
    const auto run = [](collatz::step1::Options opts) {
        std::uint64_t mod3t = 0;
        opts.on_exclude = [&mod3t](u128, unsigned, int reason) {
            mod3t += reason == Reason::reason_mod3t ? 1u : 0u;
        };
        std::vector<collatz::step1::Node> out;
        collatz::step1::enumerate(
            M, opts, [&out](const collatz::step1::Node& n) { out.push_back(n); });
        return std::make_pair(mod3t, out);
    };
    const auto base = run(o);
    const collatz::preimage::ChainTable t6(6);
    const collatz::preimage::ChainTable t4(4);
    for (const collatz::preimage::ChainTable* t : {&t6, &t4}) {
        collatz::step1::Options p = o;
        p.chain_table = t;
        const auto got = run(p);
        CHECK(got.first == base.first);
        REQUIRE(got.second.size() == base.second.size());
        for (std::size_t i = 0; i < got.second.size(); ++i) {
            CHECK(got.second[i].n0 == base.second[i].n0);
            CHECK(got.second[i].m == base.second[i].m);
            CHECK(got.second[i].f == base.second[i].f);
        }
    }
    CHECK(base.first > 0);
}
