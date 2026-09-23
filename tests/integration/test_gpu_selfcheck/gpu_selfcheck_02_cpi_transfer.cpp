// GPU self-check cases of test_integration_gpu_selfcheck: the CPI
// candidate-buffer split and the transfer path (pinned staging, copy-queue
// prefetch, trimmed uploads).

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_selfcheck.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

using namespace collatz::test::selfcheck;

// A batch whose candidates exceed the candidate-id buffer cap (2^29 ids) is
// split by survivors and each half re-run on the device.
// COLLATZ_GPU_CPI_CAND_CAP lowers the cap so N=30 overflows; one survivor's
// 2^10 candidates still fit, so no batch reaches the CPU fallback. The Result
// must equal the CPU's.
TEST_CASE("gpu cpi: a candidate-buffer overflow splits the batch") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    collatz::test::set_env("COLLATZ_GPU_CPI_CAND_CAP", "2000");
    bool used = false;
    std::string msg;
    const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
    if (collatz::test::gpu_ran(used, msg)) {
        INFO("msg " << msg);
        CHECK(msg.find(" fallback_batches=0 ") != std::string::npos);
        CHECK(msg.find(" split_batches=0 ") == std::string::npos);
        CHECK(r.gpu_selfcheck_unchecked == 0);
        check_equal(r, cpu);
    }
    collatz::test::unset_env("COLLATZ_GPU_CPI_CAND_CAP");
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}

// The transfer path (pinned staging, copy-queue prefetch into a second device
// input set, trimmed uploads, recycled batch buffers) keeps the self-check on
// every batch: a clean run, an injected readback fault and an injected lost
// tail launch (at "3": re-run; at "3,4": re-run x3 then CPU), and an injected
// lost prefetch copy (stale n0 in the prefetched input set), which only the
// prefetched-input readback check can see. Every Result scalar must equal the
// CPU verifier's.
TEST_CASE("gpu selfcheck: the transfer path rejects injected faults") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    for (const char* fault : {"none", "readback", "lost", "copy"}) {
        const std::string f(fault);
        collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST", f == "lost" ? "1" : "0");
        collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_COPY", f == "copy" ? "1" : "0");
        for (const char* inject : {"3", "3,4"}) {
            if (f != "none") {
                collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
            }
            bool used = false;
            std::string msg;
            const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
            collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
            if (!collatz::test::gpu_ran(used, msg)) {
                break;
            }
            INFO("fault " << fault << " inject " << inject << " msg " << msg);
            REQUIRE(r.gpu_selfcheck_on);
            CHECK(r.gpu_selfcheck_batches > 4);
            CHECK(r.gpu_selfcheck_unchecked == 0);
            // The transfer counters line is present, so the negative finds
            // below are not vacuous.
            CHECK(msg.find("prefetch_hits=") != std::string::npos);
            CHECK(msg.find("input_check_fail=") != std::string::npos);
            // The prefetch path ran (most batches were prefetched).
            CHECK(msg.find(" prefetch_hits=0 ") == std::string::npos);
            if (f == "copy") {
                // A stale prefetched input set is caught and the batch is
                // re-run from its host inputs (the retry uploads normally).
                CHECK(msg.find(" input_check_fail=0 ") == std::string::npos);
                CHECK(r.gpu_selfcheck_retried >= 1);
                CHECK(r.gpu_selfcheck_cpu_fallback == 0);
            } else if (f != "none" && std::string(inject) == "3") {
                CHECK(r.gpu_selfcheck_retried >= 1);
            } else if (f != "none") {
                CHECK(r.gpu_selfcheck_retried >= 3);
                CHECK(r.gpu_selfcheck_cpu_fallback >= 1);
            }
            check_equal(r, cpu);
            if (f == "none") {
                break; // one clean run
            }
        }
    }
    for (const char* e : {"COLLATZ_GPU_SELFCHECK_INJECT_LOST",
                          "COLLATZ_GPU_SELFCHECK_INJECT_COPY",
                          "COLLATZ_GPU_FLUSH_CAP"}) {
        collatz::test::unset_env(e);
    }
}
