#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "ref_random_audit.hpp"
#include "scale.hpp"

#include <doctest/doctest.h>

// The randomized ref/ audit: seeded random configs, production verify::run
// against the independent ref::verify_ref, each config run cold / cold-write /
// warm / shared-cache. ref_random_audit.hpp describes the draw, the compared
// scalars and the ref/ scope. The release build stays under the 60 s property
// budget; the sanitizer build runs a small smoke test.

TEST_CASE("audit: random configs, production == ref/ across cold/warm bv cache") {
    collatz::test::audit::Tier tier;
    tier.N_lo = 20;
    tier.N_hi = 22;
    tier.B_max = 18;
    tier.M_max = 16;
    tier.mod3t_max = 4;
    const unsigned count = collatz::test::scaled(16u, 3u);
    CHECK(collatz::test::audit::sweep(0xA5D17C0FFEEULL, count, tier) == 0u);
}
