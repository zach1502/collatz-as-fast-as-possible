// The shipped defaults of collatz/verify/defaults.hpp: the Step-2 auto-B table.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/verify.hpp"

#include <doctest/doctest.h>

using collatz::verify::auto_step2_B;

// Pins the shipped Step-2 auto-B table.
//
// When --B is not given, collatz::verify::auto_step2_B(N, use_gpu) resolves the
// look-ahead width B. The CPU path uses B=30 at 40 <= N <= 43 and the GPU path
// B=28. These tests pin that table, including the CPU B=30 and the GPU B=28 in
// [40,43].

TEST_CASE("auto_step2_B: CPU auto-B table") {
    // Below N=40: base default B=24.
    CHECK(auto_step2_B(24u, /*use_gpu=*/false) == 24u);
    CHECK(auto_step2_B(36u, false) == 24u);
    CHECK(auto_step2_B(39u, false) == 24u);
    // 40 <= N <= 43: the CPU uses B=30.
    CHECK(auto_step2_B(40u, false) == 30u);
    CHECK(auto_step2_B(41u, false) == 30u);
    CHECK(auto_step2_B(43u, false) == 30u);
    // N >= 44: B=31.
    CHECK(auto_step2_B(44u, false) == 31u);
    CHECK(auto_step2_B(48u, false) == 31u);
}

TEST_CASE("auto_step2_B: GPU stays B=28 in [40,43]") {
    CHECK(auto_step2_B(40u, /*use_gpu=*/true) == 28u);
    CHECK(auto_step2_B(43u, true) == 28u);
    // N >= 44 and N < 40 are the same on both paths.
    CHECK(auto_step2_B(44u, true) == 31u);
    CHECK(auto_step2_B(39u, true) == 24u);
    CHECK(auto_step2_B(32u, true) == 24u);
    // The CPU/GPU split exists only in [40,43].
    CHECK(auto_step2_B(40u, false) == 30u);
    CHECK(auto_step2_B(40u, true) == 28u);
}
