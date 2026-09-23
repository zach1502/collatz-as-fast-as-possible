// GPU self-check cases of test_integration_gpu_selfcheck on the Step-2 mask
// kernel: the work-group-aggregated mask scatter and a dropped mask work-group.

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_selfcheck.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

using namespace collatz::test::selfcheck;

// The work-group-aggregated mask scatter (__local atomic offsets) against the
// CPU at A=10 (L=32 lanes) and A=12 (L=128, the default from N=48). Each case
// must match the CPU on every Result scalar:
//  - clean, an injected readback fault ("3": re-run; "3,4": re-run x3 then CPU)
//    and an injected mask-count fault (INJECT_EXPAND): the aggregated
//    self-check {idsum, nonce, count} words catch every fault;
//  - a candidate-buffer cap that lands inside one work-group's aggregated
//    reservation (cap0-1 / cap0+1 ids): the per-id `idx < cand_cap` overflow
//    rule and the true cand_counter total decide the split.
TEST_CASE("gpu selfcheck: the work-group-aggregated mask scatter matches the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    for (const unsigned A : {10u, 12u}) {
        collatz::verify::Options base = base_opts();
        base.A = A;
        const collatz::verify::Result cpu = collatz::verify::run(base);
        const auto run = [&](std::string* msg) {
            bool used = false;
            const collatz::verify::Result r = collatz::gpu::verify_gpu(base, &used, msg);
            REQUIRE_MESSAGE(used, "GPU path did not run: " << *msg);
            return r;
        };
        for (const char* fault : {"none", "readback", "mask"}) {
            const std::string f(fault);
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_EXPAND", f == "mask" ? "1" : "0");
            for (const char* inject : {"3", "3,4"}) {
                if (f != "none") {
                    collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
                }
                std::string msg;
                const collatz::verify::Result r = run(&msg);
                collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
                INFO("A " << A << " fault " << fault << " inject " << inject << " msg " << msg);
                REQUIRE(r.gpu_selfcheck_on);
                CHECK(r.gpu_selfcheck_unchecked == 0);
                if (f == "none") {
                    CHECK(r.gpu_selfcheck_retried == 0);
                } else if (std::string(inject) == "3") {
                    CHECK(r.gpu_selfcheck_retried >= 1);
                } else {
                    CHECK(r.gpu_selfcheck_retried >= 3);
                    CHECK(r.gpu_selfcheck_cpu_fallback >= 1);
                }
                check_equal(r, cpu);
                if (f == "none") {
                    break;
                }
            }
        }
        collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT_EXPAND");
        // A single A=12 survivor can hold ~2^12*5/9 ids (all-ones window), so
        // its caps stay above that: one survivor never overflows.
        const int cap0 = A == 10u ? 2000 : 5000;
        for (const int dc : {-1, 1}) {
            const std::string cap = std::to_string(cap0 + dc);
            collatz::test::set_env("COLLATZ_GPU_CPI_CAND_CAP", cap.c_str());
            std::string msg;
            const collatz::verify::Result r = run(&msg);
            INFO("A " << A << " cap " << cap << " msg " << msg);
            CHECK(msg.find(" fallback_batches=0 ") != std::string::npos);
            CHECK(msg.find(" split_batches=0 ") == std::string::npos);
            CHECK(r.gpu_selfcheck_unchecked == 0);
            check_equal(r, cpu);
        }
        collatz::test::unset_env("COLLATZ_GPU_CPI_CAND_CAP");
    }
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}

// The mask reports, once per work-group, the survivors it covered in sc word 3.
// A work-group that never runs lowers the candidate count and the id sum
// consistently, so only that word can see it.
// COLLATZ_GPU_SELFCHECK_INJECT_DROPWG=1 builds the kernels with a hook that
// makes the last work-group of an injected dispatch return at once: "3" must be
// re-run on the device, "3,4" must end on the CPU, and the Result must stay
// bit-identical. The run must name the work-group check ("mask_wg_fail=").
TEST_CASE("gpu selfcheck: a dropped mask work-group is rejected") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_DROPWG", "1");
    for (const char* inject : {"", "3", "3,4"}) {
        if (inject[0] != '\0') {
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
        }
        bool used = false;
        std::string msg;
        const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
        collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
        INFO("inject '" << inject << "' msg " << msg);
        REQUIRE_MESSAGE(used, "GPU path did not run: " << msg);
        REQUIRE(r.gpu_selfcheck_on);
        CHECK(r.gpu_selfcheck_batches > 4);
        const bool wg_key = msg.find("mask_wg_fail=") != std::string::npos;
        if (inject[0] == '\0') {
            CHECK(r.gpu_selfcheck_retried == 0);
            CHECK_FALSE(wg_key);
        } else {
            if (std::string(inject) == "3") {
                CHECK(r.gpu_selfcheck_retried >= 1);
            } else {
                CHECK(r.gpu_selfcheck_retried >= 3);
                CHECK(r.gpu_selfcheck_cpu_fallback >= 1);
            }
            CHECK(wg_key);
        }
        check_equal(r, cpu);
    }
    collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT_DROPWG");
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}
