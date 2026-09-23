#define DOCTEST_CONFIG_IMPLEMENT

// GPU host producer/consumer plumbing at N=32 against the CPU verifier, with
// 4096-survivor batches (COLLATZ_GPU_FLUSH_CAP) so the run has many of them.
// The tail flags a chain when floor(max / 2^54) >= floor(threshold / 2^54), so
// while the threshold is below 2^54, as in this run, it flags every chain: a
// batch with more than 2^16 candidates overflows the flag list, also on the
// Rlb=max_bs retry, and goes to the per-survivor kernel. The Result must
// reproduce the CPU verifier's bit for bit.

#include "collatz/progress.hpp"
#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

namespace {

collatz::verify::Options base_opts() {
    collatz::verify::Options o;
    o.N = 32;
    o.A = 8;
    o.B = 18;
    o.num_bitvectors = 8;
    o.threads = 3;
    o.window_chains = 6;
    return o;
}

void check_equal(const collatz::verify::Result& g, const collatz::verify::Result& c) {
    CHECK(g.candidates_checked == c.candidates_checked);
    CHECK(g.step2_masked_out == c.step2_masked_out);
    CHECK(g.blocks_total == c.blocks_total);
    CHECK(g.coverage_checksum == c.coverage_checksum);
    CHECK(g.pow_checksum == c.pow_checksum);
    CHECK(g.max_excursion == c.max_excursion);
    CHECK(g.max_excursion_n == c.max_excursion_n);
    CHECK(g.max_exact == c.max_exact);
    // The run just checked counted each survivor finished once (the
    // --progress-file counts), on the device and on the CPU alike.
    const collatz::progress::SurvivorCounts n = collatz::progress::survivor_counts();
    CHECK(n.found > 0);
    CHECK(n.done == n.found);
}

} // namespace

TEST_CASE("gpu producer/consumer: flag-list overflows take the fallback and match the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());

    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "4096");
    bool used = false;
    std::string msg;
    const collatz::verify::Result g = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
    REQUIRE_MESSAGE(used, "device path did not run: " << msg);
    check_equal(g, cpu);
    // Flag-list overflows sent batches to the per-survivor kernel.
    INFO("msg " << msg);
    REQUIRE(msg.find(" fallback_batches=") != std::string::npos);
    CHECK(msg.find(" fallback_batches=0 ") == std::string::npos);
}
