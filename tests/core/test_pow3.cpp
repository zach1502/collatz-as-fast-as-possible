#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"

#include <cstddef>
#include <doctest/doctest.h>
#include <gmpxx.h>

using collatz::POW3;
using collatz::pow3;
using collatz::pow3_count;

TEST_CASE("POW3 table matches GMP for all 81 entries") {
    mpz_class expected = 1;
    for (std::size_t e = 0; e < pow3_count; ++e) {
        const mpz_class actual = collatz::oracle::to_mpz(POW3[e]);
        CHECK(actual == expected);
        CHECK(pow3(static_cast<unsigned>(e)) == POW3[e]);
        expected *= 3;
    }
}

TEST_CASE("POW3 basic anchors") {
    CHECK(POW3[0] == collatz::u128{1});
    CHECK(POW3[1] == collatz::u128{3});
    CHECK(POW3[2] == collatz::u128{9});
    CHECK(POW3[3] == collatz::u128{27});
    CHECK(pow3_count == 81);
    // 3^80 is a 127-bit number (verified by static_assert in the header too).
    CHECK(collatz::bit_length(POW3[80]) == 127);
}
