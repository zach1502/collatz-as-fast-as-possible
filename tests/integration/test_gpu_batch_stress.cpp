// Guards the lifetime of non-blocking writes. The device counter resets
// (candidate cursor, flag count/overflow, work cursor) are enqueued with
// clEnqueueWriteBuffer(CL_FALSE), and OpenCL requires the host source of such a
// write to stay valid until the command completes. A block-scoped stack source
// can be reused first, so a counter starts from garbage and candidates/pow come
// out wrong under load; the sources therefore live in static storage.
//
// The race cannot be asserted directly, so the test stresses the per-batch
// counter resets: a tiny COLLATZ_GPU_FLUSH_CAP turns an N=30 run into dozens of
// batches (dozens of resets), repeated three times, and each run must equal the
// CPU verifier. Reported as skipped when no device is present.

#define DOCTEST_CONFIG_IMPLEMENT

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
    o.N = 30;
    o.A = 10;
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = 4;
    o.window_chains = 6;
    return o;
}

} // namespace

TEST_CASE("gpu many-batch stress: every run equals the CPU result") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());

    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    for (int rep = 0; rep < 3; ++rep) {
        bool used = false;
        std::string msg;
        const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
        if (!collatz::test::gpu_ran(used, msg)) {
            break;
        }
        INFO("rep " << rep);
        CHECK(r.candidates_checked == cpu.candidates_checked);
        CHECK(r.step2_masked_out == cpu.step2_masked_out);
        CHECK(r.blocks_total == cpu.blocks_total);
        CHECK(r.coverage_checksum == cpu.coverage_checksum);
        CHECK(r.pow_checksum == cpu.pow_checksum);
        CHECK(r.max_excursion == cpu.max_excursion);
        CHECK(r.max_excursion_n == cpu.max_excursion_n);
        // Each survivor counted finished once (--progress-file).
        const collatz::progress::SurvivorCounts c = collatz::progress::survivor_counts();
        CHECK(c.found > 0);
        CHECK(c.done == c.found);
    }
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}
