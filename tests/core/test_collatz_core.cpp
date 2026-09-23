#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/version.hpp"

#include <doctest/doctest.h>

TEST_CASE("version string is non-empty and well formed") {
    const auto v = collatz::version_string();
    CHECK_FALSE(v.empty());
    CHECK(v == "0.1.0");
}
