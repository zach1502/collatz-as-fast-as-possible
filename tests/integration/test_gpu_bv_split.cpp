// Step-2 device-bitvector split: plan_bv_split spreads the bitvector levels over
// up to 4 device buffers when they do not fit one allocation. The parity test runs
// the GPU path with the split forced (COLLATZ_GPU_MAX_ALLOC) to 1, 2 and 4
// buffers and checks every Result scalar is bit-identical; it is reported as
// skipped when no device is present.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

namespace {

collatz::verify::Options split_opts() {
    collatz::verify::Options o;
    o.N = 28;
    o.A = 10; // M = 18
    o.B = 24; // per level 2 MiB; forced splits below.
    o.num_bitvectors = 8;
    o.threads = 2;
    o.window_chains = 6;
    return o;
}

collatz::verify::Result run_split(const char* max_alloc) {
    if (max_alloc != nullptr) {
        collatz::test::set_env("COLLATZ_GPU_MAX_ALLOC", max_alloc);
    } else {
        collatz::test::unset_env("COLLATZ_GPU_MAX_ALLOC");
    }
    bool used = false;
    std::string msg;
    const collatz::verify::Result r = collatz::gpu::verify_gpu(split_opts(), &used, &msg);
    collatz::test::unset_env("COLLATZ_GPU_MAX_ALLOC");
    REQUIRE_MESSAGE(used, "device path did not run: " << msg);
    return r;
}

} // namespace

TEST_CASE("gpu device-mask split: 1/2/4 buffers give a bit-identical Result") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    // Per level = 2^24/8 = 2 MiB. Cap 10 MiB => lpb=4 (2 buffers); 6 MiB => lpb=2
    // (4 buffers); no cap => 1 buffer.
    const collatz::verify::Result one = run_split(nullptr);
    const collatz::verify::Result two = run_split("10485760"); // 10 MiB.
    const collatz::verify::Result four = run_split("6291456"); // 6 MiB.

    for (const collatz::verify::Result* r : {&two, &four}) {
        CHECK(r->candidates_checked == one.candidates_checked);
        CHECK(r->step2_masked_out == one.step2_masked_out);
        CHECK(r->blocks_total == one.blocks_total);
        CHECK(r->coverage_checksum == one.coverage_checksum);
        CHECK(r->pow_checksum == one.pow_checksum);
        CHECK(r->max_excursion == one.max_excursion);
        CHECK(r->max_excursion_n == one.max_excursion_n);
        CHECK(r->max_exact == one.max_exact);
    }
}
