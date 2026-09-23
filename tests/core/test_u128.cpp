#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/u128.hpp"

#include <doctest/doctest.h>
#include <string>

using collatz::u128;
using collatz::u128_max;

namespace {

constexpr u128 one = 1;
constexpr u128 two_127 = one << 127;

} // namespace

TEST_CASE("make/hi/lo round trip") {
    const u128 x = collatz::make_u128(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
    CHECK(collatz::hi64(x) == 0x0123456789abcdefULL);
    CHECK(collatz::lo64(x) == 0xfedcba9876543210ULL);
}

TEST_CASE("ctz edge cases") {
    CHECK(collatz::ctz(0) == 128); // defined convention: no set bit
    CHECK(collatz::ctz(1) == 0);
    CHECK(collatz::ctz(2) == 1);
    CHECK(collatz::ctz(one << 63) == 63);
    CHECK(collatz::ctz(one << 64) == 64);
    CHECK(collatz::ctz(one << 127) == 127);
    CHECK(collatz::ctz(u128_max) == 0);
}

TEST_CASE("clz and bit_length edge cases") {
    CHECK(collatz::clz(0) == 128);
    CHECK(collatz::clz(1) == 127);
    CHECK(collatz::clz(one << 127) == 0);
    CHECK(collatz::clz(u128_max) == 0);

    CHECK(collatz::bit_length(0) == 0);
    CHECK(collatz::bit_length(1) == 1);
    CHECK(collatz::bit_length(2) == 2);
    CHECK(collatz::bit_length(one << 64) == 65);
    CHECK(collatz::bit_length(two_127) == 128);
    CHECK(collatz::bit_length(u128_max) == 128);
}

TEST_CASE("checked_add boundaries") {
    bool ovf = false;
    CHECK(collatz::checked_add(u128_max, 0, ovf) == u128_max);
    CHECK_FALSE(ovf);

    (void) collatz::checked_add(u128_max, 1, ovf);
    CHECK(ovf);

    const u128 r = collatz::checked_add(u128_max, 1, ovf);
    CHECK(ovf);
    CHECK(r == 0); // wraps to zero
}

TEST_CASE("checked_mul boundaries") {
    bool ovf = false;
    // (2^128 - 1) / 3, times 3, must not overflow.
    const u128 third = u128_max / 3;
    const u128 prod = collatz::checked_mul(third, 3, ovf);
    CHECK_FALSE(ovf);
    CHECK(prod == third * 3);

    // u128_max % 3 == 0, so (third + 1) * 3 = u128_max + 3 overflows.
    (void) collatz::checked_mul(third + 1, 3, ovf);
    CHECK(ovf);

    (void) collatz::checked_mul(u128_max, 2, ovf);
    CHECK(ovf);

    CHECK(collatz::checked_mul(u128_max, 0, ovf) == 0);
    CHECK_FALSE(ovf);
}

TEST_CASE("decimal round trip") {
    CHECK(collatz::to_string(0) == "0");
    CHECK(collatz::to_string(1) == "1");
    CHECK(collatz::to_string(u128_max) == "340282366920938463463374607431768211455");

    struct Case {
        u128 value;
    };
    const u128 samples[] = {0,
                            1,
                            2,
                            9,
                            10,
                            99,
                            100,
                            1000000000000000000ULL,
                            two_127,
                            u128_max,
                            u128_max - 1,
                            (one << 100) + 7};
    for (const u128 v : samples) {
        u128 parsed = 0;
        REQUIRE(collatz::from_string(collatz::to_string(v), parsed));
        CHECK(parsed == v);
    }
}

TEST_CASE("from_string rejects bad input and overflow") {
    u128 out = 0;
    CHECK_FALSE(collatz::from_string("", out));
    CHECK_FALSE(collatz::from_string("12a3", out));
    CHECK_FALSE(collatz::from_string("-1", out));
    CHECK(collatz::from_string("+42", out));
    CHECK(out == 42);

    // 2^128 exactly (one past max) must overflow.
    CHECK_FALSE(collatz::from_string("340282366920938463463374607431768211456", out));
    // A number with more digits than u128_max overflows.
    CHECK_FALSE(collatz::from_string("999999999999999999999999999999999999999999", out));
}

TEST_CASE("canonical_decimal and decimal_less order integers of any size") {
    std::string s;
    REQUIRE(collatz::canonical_decimal("000123", s));
    CHECK(s == "123");
    REQUIRE(collatz::canonical_decimal("+0", s));
    CHECK(s == "0");
    CHECK_FALSE(collatz::canonical_decimal("", s));
    CHECK_FALSE(collatz::canonical_decimal("12a", s));
    CHECK_FALSE(collatz::canonical_decimal("-5", s));
    CHECK(collatz::decimal_less("99", "100"));
    CHECK(collatz::decimal_less("340282366920938463463374607431768211455",
                                "340282366920938463463374607431768211456"));
    CHECK_FALSE(collatz::decimal_less("5", "5"));
    CHECK_FALSE(collatz::decimal_less("1000", "999"));
}
