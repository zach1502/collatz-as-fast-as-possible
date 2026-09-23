#define DOCTEST_CONFIG_IMPLEMENT

// The OpenCL verifier on a small run (N=24) reproduces the CPU Result. Reported
// as skipped when no OpenCL device is present.

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu

#include <doctest/doctest.h>
#include <string>

namespace {

collatz::verify::Options make_opts() {
    collatz::verify::Options o;
    o.N = 24;
    o.A = 8;  // M = 16
    o.B = 18; // 2^18-bit bitvectors: a small filter.
    o.num_bitvectors = 8;
    o.threads = 2;
    o.window_chains = 6; // the default; masks inherit it through the Filter
    return o;
}

} // namespace

TEST_CASE("gpu verify reproduces the CPU Result at N=24") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Options o = make_opts();
    const collatz::verify::Result cpu = collatz::verify::run(o);

    bool used_gpu = false;
    std::string msg;
    const collatz::verify::Result gpu = collatz::gpu::verify_gpu(o, &used_gpu, &msg);
    REQUIRE_MESSAGE(used_gpu, "device path did not run: " << msg);

    CHECK(gpu.candidates_checked == cpu.candidates_checked);
    CHECK(gpu.step2_masked_out == cpu.step2_masked_out);
    CHECK(gpu.blocks_total == cpu.blocks_total);
    CHECK(gpu.coverage_checksum == cpu.coverage_checksum);
    CHECK(gpu.pow_checksum == cpu.pow_checksum);
    CHECK(gpu.max_excursion == cpu.max_excursion);
    CHECK(gpu.max_excursion_n == cpu.max_excursion_n);
    CHECK(gpu.max_exact == cpu.max_exact);
}
