#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <vector>

using collatz::u128;
using collatz::ref::Excl;
using collatz::ref::Options;
using collatz::ref::Survivor;

namespace {

// Independent GMP oracle, separate from the ref library's own arithmetic.
[[nodiscard]] mpz_class ind_T(const mpz_class& x) {
    if (mpz_even_p(x.get_mpz_t())) {
        return x / 2;
    }
    return (3 * x + 1) / 2;
}

// The property sweep is O(2^M): M=20 in optimized builds, M=16 under the
// sanitizers.
const unsigned kMainM = collatz::test::scaled(20, 16);

} // namespace

TEST_CASE("survivors(M) count matches an independent classify recount") {
    const Options all;
    const std::vector<Survivor> surv = collatz::ref::survivors(kMainM, all);
    MESSAGE("survivors(" << kMainM << ") count = " << surv.size());

    // An independent second pass counts Excl::none directly with classify().
    std::uint64_t recount = 0;
    const std::uint64_t total = std::uint64_t{1} << kMainM;
    for (std::uint64_t v = 0; v < total; ++v) {
        if (collatz::ref::classify(static_cast<u128>(v), kMainM, all).by == Excl::none) {
            ++recount;
        }
    }
    CHECK(recount == surv.size());
}

TEST_CASE("every survivor's terminal node m and odd-count f are independently correct") {
    const Options all;
    const std::vector<Survivor> surv = collatz::ref::survivors(kMainM, all);
    REQUIRE(!surv.empty());

    for (const Survivor& s : surv) {
        // Recompute m = T^M(n0) and f = f_M from scratch with the GMP oracle.
        mpz_class x = collatz::oracle::to_mpz(s.n0);
        unsigned f = 0;
        for (unsigned i = 0; i < kMainM; ++i) {
            if (mpz_odd_p(x.get_mpz_t())) {
                ++f;
            }
            x = ind_T(x);
        }
        CHECK(collatz::oracle::to_mpz(s.m) == x);
        CHECK(s.f == f);
    }

    // Survivors must be strictly ascending in n0 and all == 3 (mod 4).
    for (std::size_t i = 0; i < surv.size(); ++i) {
        CHECK((surv[i].n0 % 4) == 3);
        if (i > 0) {
            CHECK(surv[i - 1].n0 < surv[i].n0);
        }
    }
}

TEST_CASE("survivor counts across M (pinned)") {
    const Options all;
    // Small M only, so the release run stays under 20 s; the self-consistency
    // case above covers survivors(20). The pins were recounted independently,
    // by a short brute-force program of the rules in ref_step1.hpp (descent,
    // path merge and odd-even-even), which also gives the hand-verified 3 and 4
    // survivors of test_ref_step1.cpp at M=4 and 5.
    struct Pin {
        unsigned M;
        std::size_t count;
    };
    for (const Pin& p : {Pin{8, 18}, Pin{12, 174}, Pin{16, 1646}}) {
        const std::size_t got = collatz::ref::survivors(p.M, all).size();
        MESSAGE("survivors(" << p.M << ") = " << got);
        CHECK(got == p.count);
    }
}
