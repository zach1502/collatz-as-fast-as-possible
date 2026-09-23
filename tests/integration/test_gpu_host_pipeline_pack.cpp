// Producer-side input packing and the staged bitvector upload of the --gpu host
// pipeline against the CPU verifier. Reported as skipped when no OpenCL device
// is present or a run falls back to the CPU.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/verify.hpp"
#include "gpu_pipeline.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

using namespace collatz::test::gpu_pipeline;

// Producers pack each batch's staged upload into a pool block, and the
// dispatch thread uploads straight from it. Bit-identity vs the CPU with the
// default and a forced split cap and under the self-check injections.
TEST_CASE("producer-side input packing is bit-identical to the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    struct Cfg {
        const char* cap;   // "0" = default id cap
        const char* fault; // "" = none, "3", "3,4", "copy", "lost"
    };
    const Cfg cfgs[] = {
        {"0", ""},
        {"20000", ""},
        {"0", "3"},
        {"0", "3,4"},
        {"0", "copy"},
        {"0", "lost"},
        {"20000", "3,4"},
    };
    for (const Cfg& c : cfgs) {
        clear_all();
        if (std::string(c.cap) != "0") {
            collatz::test::set_env("COLLATZ_GPU_CPI_CAND_CAP", c.cap);
        }
        const std::string f = c.fault;
        if (!f.empty()) {
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT",
                                   (f == "copy" || f == "lost") ? "3" : c.fault);
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_COPY", f == "copy" ? "1" : "0");
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST", f == "lost" ? "1" : "0");
        }
        collatz::verify::Result r;
        if (!run_gpu(base_opts(), r)) {
            clear_all();
            return;
        }
        INFO("cap " << c.cap << " fault '" << f << "'");
        check_equal(r, cpu);
    }
    clear_all();
}

TEST_CASE("EARLY_UPLOAD: a failed staged upload falls back to the level upload, bit-identical") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    // "inject": the test hook throws after chunk 0's write is enqueued (device
    // buffers created, unused slots aliasing slot 0, a write in flight).
    // "huge": a 1 TiB staging chunk, which no device can allocate. Either way
    // the staged upload throws, its cleanup must release each device buffer
    // once, and the fallback to the per-level upload must give the same Result.
    for (const char* how : {"inject", "huge"}) {
        clear_all();
        const std::string h = how;
        collatz::test::set_env("COLLATZ_GPU_EARLY_UPLOAD_INJECT_FAIL", h == "inject" ? "1" : "0");
        collatz::test::set_env("COLLATZ_GPU_EARLY_UPLOAD_CHUNK_MB", h == "huge" ? "1048576" : "1");
        collatz::verify::Result r;
        if (!run_gpu(base_opts(), r)) {
            clear_all();
            return;
        }
        INFO("failure " << h);
        check_equal(r, cpu);
        CHECK_FALSE(r.gpu_upload.early_upload); // fell back
        CHECK(r.gpu_upload.upload_chunks == 0u);
    }
    clear_all();
}
