#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "mpz_doctest.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <vector>

using collatz::u128;
using collatz::ref::Excl;
using collatz::ref::Options;
using collatz::ref::Survivor;

namespace {

// GMP oracle that does not call the ref library: one accelerated Collatz step
// and its k-fold iterate, so the checks below compare ref with a separate code
// path.

[[nodiscard]] mpz_class ind_T(const mpz_class& x) {
    if (mpz_even_p(x.get_mpz_t())) {
        return x / 2;
    }
    return (3 * x + 1) / 2;
}

[[nodiscard]] mpz_class ind_iterate(const mpz_class& x, unsigned steps) {
    mpz_class y = x;
    for (unsigned i = 0; i < steps; ++i) {
        y = ind_T(y);
    }
    return y;
}

[[nodiscard]] mpz_class n0_mpz(std::uint64_t v) {
    return collatz::oracle::to_mpz(static_cast<u128>(v));
}

/// A hand-computed expected survivor row.
struct Expected {
    std::uint64_t n0;
    std::uint64_t m;
    unsigned f;
};

void check_survivors_equal(unsigned M, const std::vector<Expected>& expected) {
    const Options all; // descent + path_merge + oee
    const std::vector<Survivor> got = collatz::ref::survivors(M, all);
    REQUIRE(got.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(got[i].n0 == static_cast<u128>(expected[i].n0));
        CHECK(got[i].m == static_cast<u128>(expected[i].m));
        CHECK(got[i].f == expected[i].f);
    }
}

} // namespace

// Hand-verified small cases.
//
// Only n0 == 3 (mod 4) can survive (even -> descent@1; n0==1 mod4 -> descent@2).
// Below, each trajectory is m_0,m_1,... under T(n)=n/2 or (3n+1)/2. OEE needs
// depth k >= 5, so at M=4 only descent / path-merge can fire.

TEST_CASE("hand-verified survivors at M=4") {
    // Candidates 3,7,11,15:
    //   3 : 3,5,8,4,2         -> descent@4 (m_4=2 < 3)                excluded
    //   7 : 7,11,17,26,13     -> no hit                              survivor (m=13, f=3)
    //  11 : 11,17,26,13,20    -> no hit                              survivor (m=20, f=3)
    //  15 : 15,23,35,53,80    -> no hit                              survivor (m=80, f=4)
    // f counts odd m_0..m_3: 7->{7,11,17}=3; 11->{11,17,13}=3; 15->{15,23,35,53}=4.
    check_survivors_equal(4, {{7, 13, 3}, {11, 20, 3}, {15, 80, 4}});
}

TEST_CASE("hand-verified survivors at M=5") {
    // At M=5 the checks also test k=5. Re-examining the M=4 survivors:
    //   7 : ...,m_5=20         -> no hit                              survivor (m=20, f=4)
    //  11 : ...,m_5=10 < 11    -> descent@5                           excluded
    //  15 : ...,m_5=40         -> no hit                              survivor (m=40, f=4)
    // New candidates 19,23 descend; 27 and 31 survive:
    //  27 : 27,41,62,31,47,71  -> no hit                              survivor (m=71, f=4)
    //  31 : 31,47,71,107,161,242 -> no hit                            survivor (m=242, f=5)
    check_survivors_equal(5, {{7, 20, 4}, {15, 40, 4}, {27, 71, 4}, {31, 242, 5}});
}

TEST_CASE("survivor sets shrink monotonically with M on the shared range") {
    // A survivor at M+1 passed strictly more checks than at M, so any survivor
    // of M+1 with n0 < 2^M must also be a survivor of M.
    const Options all;
    for (unsigned M = 4; M <= 9; ++M) {
        const std::vector<Survivor> small = collatz::ref::survivors(M, all);
        const std::vector<Survivor> big = collatz::ref::survivors(M + 1, all);
        std::vector<u128> small_set;
        small_set.reserve(small.size());
        for (const auto& s : small) {
            small_set.push_back(s.n0);
        }
        const u128 bound = static_cast<u128>(1) << M;
        for (const auto& b : big) {
            if (b.n0 < bound) {
                const bool present =
                    std::find(small_set.begin(), small_set.end(), b.n0) != small_set.end();
                CHECK_MESSAGE(present,
                              "survivor " << collatz::to_string(b.n0) << " of M=" << (M + 1)
                                          << " missing from M=" << M);
            }
        }
    }
}

TEST_CASE("descent sanity: T^depth(n0) < n0 for descent-classified n0") {
    const Options all;
    long long checked = 0;
    for (std::uint64_t v = 2; v < 256; ++v) {
        const auto c = collatz::ref::classify(static_cast<u128>(v), 32, all);
        if (c.by != Excl::descent) {
            continue;
        }
        const mpz_class start = n0_mpz(v);
        const mpz_class landed = ind_iterate(start, c.depth);
        CHECK_MESSAGE(landed < start,
                      "n0=" << v << " depth=" << c.depth
                            << " landed=" << collatz::oracle::to_decimal(landed));
        ++checked;
    }
    CHECK(checked > 0);
    MESSAGE("descent-sanity checked " << checked << " values");
}

TEST_CASE("path-merge sanity: T((2m-1)/3) == m and predecessor < n0") {
    const Options all;
    long long checked = 0;
    for (std::uint64_t v = 2; v < 4096; ++v) {
        const auto c = collatz::ref::classify(static_cast<u128>(v), 40, all);
        if (c.by != Excl::path_merge) {
            continue;
        }
        const mpz_class start = n0_mpz(v);
        const mpz_class mk = ind_iterate(start, c.depth);
        const mpz_class mk_mod3 = mk % 3; // materialize: keep gmp expr templates out of CHECK
        REQUIRE(mk_mod3 == 2);
        const mpz_class pred = (2 * mk - 1) / 3;
        CHECK(pred < start);      // it is a strictly smaller predecessor
        CHECK(ind_T(pred) == mk); // that maps forward to the same node
        ++checked;
    }
    CHECK(checked > 0);
    MESSAGE("path-merge-sanity checked " << checked << " values");
}

TEST_CASE("OEE sanity: Hercher Lemma-9 merge identity holds for every witness") {
    // For an OEE hit at depth k with witness j:
    //   T^{k-j}(m_j) == T^{k-j-1}((m_j - 1)/2),   and   (m_j - 1)/2 < n0.
    // Verified with the independent GMP oracle above.
    const Options all;
    constexpr unsigned kM = 48; // generous depth cap so OEE fires at its natural depth
    long long checked = 0;
    for (std::uint64_t v = 3; v < (std::uint64_t{1} << 15); ++v) {
        unsigned j = 0;
        unsigned k = 0;
        if (!collatz::ref::oee_witness(static_cast<u128>(v), kM, all, j, k)) {
            continue;
        }
        REQUIRE(j >= 2);
        REQUIRE(j <= k - 3);
        const mpz_class start = n0_mpz(v);
        const mpz_class mj = ind_iterate(start, j);
        REQUIRE(mpz_odd_p(mj.get_mpz_t())); // witness node is odd
        const mpz_class pred = (mj - 1) / 2;
        CHECK(pred < start);

        const mpz_class lhs = ind_iterate(mj, k - j);
        const mpz_class rhs = ind_iterate(pred, k - j - 1);
        CHECK_MESSAGE(lhs == rhs,
                      "n0=" << v << " k=" << k << " j=" << j
                            << " lhs=" << collatz::oracle::to_decimal(lhs)
                            << " rhs=" << collatz::oracle::to_decimal(rhs));

        // Trailing two steps are even (the "EE" of OEE).
        CHECK_FALSE(mpz_odd_p(ind_iterate(start, k - 2).get_mpz_t()));
        CHECK_FALSE(mpz_odd_p(ind_iterate(start, k - 1).get_mpz_t()));
        ++checked;
    }
    CHECK_MESSAGE(checked > 0, "expected at least one OEE-classified n0 in range");
    MESSAGE("OEE Hercher-sanity checked " << checked << " witnesses");
}
