// GPU self-check cases of test_integration_gpu_selfcheck: the split sub-table
// lookup in the tail and the two-deep in-flight pipeline.

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_selfcheck.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

using namespace collatz::test::selfcheck;

// The split sub-table lookup (split 8, staged in __local) in
// tail_persistent's slot loop, clean and under readback and lost-launch
// faults. Every Result scalar must equal the CPU's (the composed {3^f, c} is
// TBL16's; the block size is 16).
TEST_CASE("gpu selfcheck: the split table tail folds every candidate exactly once") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    for (const char* fault : {"none", "readback", "lost"}) {
        const std::string f(fault);
        collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST", f == "lost" ? "1" : "0");
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
            INFO("fault " << fault << " inject " << inject);
            CHECK(msg.find("u64n=1 split=8") != std::string::npos);
            REQUIRE(r.gpu_selfcheck_on);
            CHECK(r.gpu_selfcheck_batches > 4);
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
    collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST");
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}

// The two-deep in-flight pipeline. Validation of batch i happens when its
// readback lands while batch i+1 runs; each fault must be rejected exactly as a
// serial dispatch rejects it ("3": re-run; "3,4": re-run x3 then CPU -- the injection
// sequence is in validation order, so a batch still in flight is discarded and
// resubmitted when a re-run validates first): a readback fault, a bad mask
// count (INJECT_EXPAND), a lost tail launch (INJECT_LOST), a dropped mask
// work-group (INJECT_DROPWG) and a lost prefetch copy (INJECT_COPY); plus a
// candidate-buffer overflow that must split. Every Result scalar must equal
// the CPU's.
TEST_CASE("gpu selfcheck: the two-deep in-flight pipeline rejects injected faults") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    const auto count = [](const std::string& msg, const char* key) -> unsigned long long {
        const std::size_t at = msg.find(key);
        return at == std::string::npos ? 0ULL
                                       : std::stoull(msg.substr(at + std::string(key).size()));
    };
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
    struct Fault {
        const char* name;
        const char* env; // extra knob for the run (nullptr = none)
    };
    const Fault faults[] = {
        {"clean", nullptr},
        {"readback", nullptr},
        {"expand", "COLLATZ_GPU_SELFCHECK_INJECT_EXPAND"},
        {"lost", "COLLATZ_GPU_SELFCHECK_INJECT_LOST"},
        {"dropwg", "COLLATZ_GPU_SELFCHECK_INJECT_DROPWG"},
        {"copy", "COLLATZ_GPU_SELFCHECK_INJECT_COPY"},
        {"split", "COLLATZ_GPU_CPI_CAND_CAP"},
    };
    for (const Fault& f : faults) {
        const std::string fn = f.name;
        for (const char* inject : {"3", "3,4"}) {
            if (f.env != nullptr) {
                collatz::test::set_env(f.env, fn == "split" ? "2000" : "1");
            }
            const bool injected = fn != "clean" && fn != "split";
            if (injected) {
                collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
            }
            bool used = false;
            std::string msg;
            const collatz::verify::Result r = collatz::gpu::verify_gpu(base_opts(), &used, &msg);
            for (const char* e : {f.env, "COLLATZ_GPU_SELFCHECK_INJECT"}) {
                if (e != nullptr) {
                    collatz::test::unset_env(e);
                }
            }
            INFO("fault " << fn << " inject " << inject << " msg " << msg);
            REQUIRE_MESSAGE(used, "GPU path did not run: " << msg);
            REQUIRE(r.gpu_selfcheck_on);
            CHECK(r.gpu_selfcheck_batches > 4);
            CHECK(r.gpu_selfcheck_unchecked == 0);
            // The pipeline ran (most batches were submitted in flight).
            CHECK(count(msg, "inflight_batches=") > 4u);
            if (fn == "split") {
                CHECK(msg.find(" split_batches=0 ") == std::string::npos);
                CHECK(msg.find(" fallback_batches=0 ") != std::string::npos);
            } else if (fn == "copy") {
                CHECK(msg.find(" input_check_fail=0 ") == std::string::npos);
                CHECK(r.gpu_selfcheck_retried >= 1);
                CHECK(r.gpu_selfcheck_cpu_fallback == 0);
            } else if (injected && std::string(inject) == "3") {
                CHECK(r.gpu_selfcheck_retried >= 1);
            } else if (injected) {
                CHECK(r.gpu_selfcheck_retried >= 3);
                CHECK(r.gpu_selfcheck_cpu_fallback >= 1);
            }
            if (fn == "dropwg") {
                CHECK(msg.find("mask_wg_fail=") != std::string::npos);
            }
            check_equal(r, cpu);
            if (!injected) {
                break; // one run per non-injected case
            }
        }
    }
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
}
