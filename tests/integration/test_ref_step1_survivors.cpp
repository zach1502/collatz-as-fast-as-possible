#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "scale.hpp"

#include <doctest/doctest.h>
#include <vector>

// Cross-implementation equality: the optimized Step-1 kernel
// (<collatz/step1.hpp>) must agree, survivor for survivor, with this slow
// reference. The test compiles with or without the optimized header:
//   * header present -> comparison over several M, with and without mod3t
//   * header absent  -> a single passing case that reports the skip
//
// The sweep is small (M up to 16 and a lighter mod3t sweep), smaller still under
// sanitizers.
//
// The comparison relies only on the documented API, not on the optimized
// source: namespace collatz::step1 with struct Options (unsigned mod3t), struct
// Node (u128 n0 and m, unsigned f) and enumerate(M, options, sink), which calls
// the sink once per survivor.
#if __has_include(<collatz/step1.hpp>)
#include <collatz/step1.hpp>
#define COLLATZ_HAVE_STEP1 1
#else
#define COLLATZ_HAVE_STEP1 0
#endif

#if COLLATZ_HAVE_STEP1

#include <algorithm>

namespace {

using collatz::u128;
using collatz::test::sanitized;

// Descent, path merge and OEE (the reference's defaults and the only sieves the
// optimized kernel runs), plus the mod-3^t sieve when mod3t > 0.
void compare_one(unsigned M, unsigned mod3t) {
    collatz::ref::Options ref_opts;
    ref_opts.mod3t = mod3t;

    collatz::step1::Options opt_opts;
    opt_opts.mod3t = mod3t;

    const auto ref_surv = collatz::ref::survivors(M, ref_opts);
    std::vector<collatz::step1::Node> opt_surv;
    collatz::step1::enumerate(
        M, opt_opts, [&opt_surv](const collatz::step1::Node& n) { opt_surv.push_back(n); });
    std::sort(
        opt_surv.begin(), opt_surv.end(), [](const auto& a, const auto& b) { return a.n0 < b.n0; });

    CHECK_MESSAGE(ref_surv.size() == opt_surv.size(),
                  "M=" << M << " mod3t=" << mod3t << " ref=" << ref_surv.size()
                       << " opt=" << opt_surv.size());

    const std::size_t n = std::min(ref_surv.size(), opt_surv.size());
    for (std::size_t i = 0; i < n; ++i) {
        const bool eq = static_cast<u128>(opt_surv[i].n0) == ref_surv[i].n0 &&
                        static_cast<u128>(opt_surv[i].m) == ref_surv[i].m &&
                        static_cast<unsigned>(opt_surv[i].f) == ref_surv[i].f;
        CHECK_MESSAGE(eq,
                      "M=" << M << " mod3t=" << mod3t << " row " << i
                           << " n0=" << collatz::to_string(ref_surv[i].n0));
    }
}

} // namespace

TEST_CASE("optimized Step-1 survivors match the reference (smoke across M and subsets)") {
    // Without mod3t at several M: up to M=16, or M=12 under the sanitizers.
    const std::vector<unsigned> base_ms =
        sanitized() ? std::vector<unsigned>{8U, 10U, 12U} : std::vector<unsigned>{8U, 12U, 16U};
    for (const unsigned M : base_ms) {
        compare_one(M, 0);
    }

    // The mod3t sieve at t in {2,3}. The reference's per-node preimage BFS costs
    // O(2^M * 3^t), so M stays small.
    const std::vector<unsigned> t3_ms =
        sanitized() ? std::vector<unsigned>{8U, 10U} : std::vector<unsigned>{8U, 12U};
    for (const unsigned M : t3_ms) {
        for (const unsigned t : {2U, 3U}) {
            compare_one(M, t);
        }
    }
}

#else // !COLLATZ_HAVE_STEP1

TEST_CASE("optimized Step-1 cross-check (skipped)") {
    MESSAGE("step1 optimized not present");
    CHECK(true);
}

#endif // COLLATZ_HAVE_STEP1
