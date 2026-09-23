// The GPU per-batch self-check detects a corrupted batch, re-runs it, and falls
// back to the CPU after 3 failed re-runs, with a bit-identical Result.
//
// COLLATZ_GPU_SELFCHECK_INJECT=<dispatch>[,<count>] flips one word of the
// host-side readback of `count` consecutive self-checked dispatches, which is
// what a stale or short device result looks like to the host. Reported as
// skipped when no device is present or a run falls back to the CPU.
//
// The executable also compiles the cases in test_gpu_selfcheck/: the CPI split
// and the transfer path (02), the mask work-groups (03), and the split table
// tail and the in-flight pipeline (04).

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_selfcheck.hpp"
#include "gpu_test_main.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

using namespace collatz::test::selfcheck;

TEST_CASE("gpu selfcheck: an injected bad batch is re-run or recomputed on the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());

    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    // "3": one corrupted dispatch -> detected and re-run on the device.
    // "3,4": the batch and all 3 re-runs corrupted -> computed on the CPU.
    for (const char* inject : {"3", "3,4"}) {
        collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
        bool used = false;
        std::string msg;
        const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
        collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
        if (!collatz::test::gpu_ran(used, msg)) {
            break;
        }
        INFO("inject " << inject);
        REQUIRE(r.gpu_selfcheck_on);
        CHECK(r.gpu_selfcheck_batches > 4);
        CHECK(r.gpu_selfcheck_unchecked == 0);
        if (std::string(inject) == "3") {
            CHECK(r.gpu_selfcheck_retried >= 1);
        } else {
            CHECK(r.gpu_selfcheck_retried >= 3);
            CHECK(r.gpu_selfcheck_cpu_fallback >= 1);
        }
        check_equal(r, cpu);
    }
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}

// An injected tail fault ("3", "3,4") and an injected fault in the mask's
// deferred count word (COLLATZ_GPU_SELFCHECK_INJECT_EXPAND) must be rejected and
// re-run or recomputed on the CPU, with a bit-identical Result.
TEST_CASE("gpu selfcheck: injected tail and mask-count faults are rejected") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());

    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    for (const char* fault : {"tail", "mask"}) {
        const bool mask_fault = std::string(fault) == "mask";
        collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_EXPAND", mask_fault ? "1" : "0");
        for (const char* inject : {"", "3", "3,4"}) {
            if (inject[0] != '\0') {
                collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
            }
            bool used = false;
            std::string msg;
            const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
            collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
            if (!collatz::test::gpu_ran(used, msg)) {
                break;
            }
            INFO("fault " << fault << " inject '" << inject << "'");
            REQUIRE(r.gpu_selfcheck_on);
            CHECK(r.gpu_selfcheck_batches > 4);
            CHECK(r.gpu_selfcheck_unchecked == 0);
            if (inject[0] == '\0') {
                CHECK(r.gpu_selfcheck_retried == 0);
            } else if (std::string(inject) == "3") {
                CHECK(r.gpu_selfcheck_retried >= 1);
            } else {
                CHECK(r.gpu_selfcheck_retried >= 3);
                CHECK(r.gpu_selfcheck_cpu_fallback >= 1);
            }
            check_equal(r, cpu);
        }
    }
    collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT_EXPAND");
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}
