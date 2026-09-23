// The CPU verifier's fold internals on inputs its validated runs do not reach
// (src/core/verify_survivor.hpp, verify_fold.hpp, verify_records.cpp): the u64
// lane test for m + 3^f * a, and a record contender whose prefix holds an odd
// value w with 3w + 1 >= 2^128 while every value stays below 2^128.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "frontier_model/walk.hpp"
#include "verify_emit.hpp"
#include "verify_internal.hpp"
#include "verify_survivor.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>
#include <memory>
#include <string>

namespace vd = collatz::verify::detail;
using collatz::u128;
using collatz::frontier_test::excursion_dec;
using collatz::frontier_test::T_mpz;

TEST_CASE("madd_fits_u64 keeps a multiply overflow that the add does not carry") {
    // 3^76 * (2^14 - 1) passes 2^128 and adding 3^76 - 1 to the wrapped
    // product does not carry: the all-odd survivor at f = 76, A = 14.
    CHECK_FALSE(vd::madd_fits_u64(collatz::pow3(76) - 1, collatz::pow3(76), (1u << 14) - 1));
    // 2^127 * 2 wraps to 0, which leaves 5 below 2^64.
    CHECK_FALSE(vd::madd_fits_u64(5, static_cast<u128>(1) << 127, 2));
    CHECK_FALSE(vd::madd_fits_u64(collatz::u128_max, 1, 1));
    const u128 two64 = static_cast<u128>(1) << 64;
    CHECK(vd::madd_fits_u64(two64 - 7, 3, 2));
    CHECK_FALSE(vd::madd_fits_u64(two64 - 6, 3, 2));
    CHECK(vd::madd_fits_u64(5, 3, 0));
}

TEST_CASE("a prefix value past (2^128-1)/3 below 2^128 is no overflow fallback") {
    u128 n = 0; // 0x8fea249a11af8d14cabffffffff
    REQUIRE(collatz::from_string("182433455691810943193223826243583", n));
    const unsigned M = 42;
    const mpz_class third = collatz::oracle::to_mpz(collatz::u128_max / 3);
    mpz_class w = collatz::oracle::to_mpz(n);
    mpz_class pmax = w;
    bool big3 = false;
    for (unsigned j = 0; j < M; ++j) {
        big3 = big3 || (mpz_odd_p(w.get_mpz_t()) != 0 && w >= third);
        w = T_mpz(w);
        REQUIRE(mpz_sizeinbase(w.get_mpz_t(), 2) <= 128);
        pmax = w > pmax ? w : pmax;
    }
    REQUIRE(big3); // T on that w leaves u128 in 3w + 1, so the u128 walks replay it in GMP
    u128 m = 0;
    u128 pm = 0;
    REQUIRE(collatz::from_string(collatz::oracle::to_decimal(w), m));
    REQUIRE(collatz::from_string(collatz::oracle::to_decimal(pmax), pm));
    const std::string q = excursion_dec(n);
    for (const bool listed : {false, true}) {
        CAPTURE(listed);
        vd::RecordThreshold rt;
        u128 x = 0;
        REQUIRE(collatz::from_string(q, x));
        rt.fits_u128 = true;
        rt.x = x - 1;
        rt.dec = collatz::to_string(rt.x);
        auto acc = std::make_unique<vd::Acc>();
        if (listed) {
            acc->rt = &rt;
            acc->bar_cap = x;
        }
        auto tb = std::make_unique<vd::TailBatch>();
        vd::push_tail_batch(*acc, *tb, M, 16, n, m, pm, 0, 0, true);
        vd::flush_tail_batch(*acc, *tb, M, 16);
        vd::drain_defer_queue(*acc, tb->dq, 16);
        CHECK(acc->candidates == 1);
        CHECK(acc->overflow == 0); // no value of the walk passes 2^128-1
        CHECK_FALSE(acc->max_exceeds);
        CHECK(acc->wide_v.empty());
        REQUIRE(acc->have_max);
        CHECK(collatz::to_string(acc->maxv) == q);
        CHECK(acc->maxn == n);
        if (listed) {
            REQUIRE(acc->rec_hits.size() >= 1);
            for (const auto& h : acc->rec_hits) {
                CHECK(h.first == n);
                CHECK(collatz::to_string(h.second) == q);
            }
        }
    }
}
