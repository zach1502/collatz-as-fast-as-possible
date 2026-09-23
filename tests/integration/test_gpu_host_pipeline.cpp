// Host pipeline of the --gpu path: when a device is present, bit-identity with
// the CPU verifier. A small flush cap makes a tiny N run many batches, a small
// candidate cap (COLLATZ_GPU_CPI_CAND_CAP) forces candidate-cap splits, whose
// counters show that the split halves went through cpi_submit, and injections
// cover the self-check retry, CPU-fallback, lost-copy and lost-launch paths.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/verify.hpp"
#include "gpu_pipeline.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>

using namespace collatz::test::gpu_pipeline;

TEST_CASE("host pipeline: the in-flight pipeline is bit-identical to the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    // A small id cap splits most batches, so the pipelined split and the
    // early prefetch and pre-pack interplay with infl_redo run; "0" = the
    // default cap.
    for (const char* cap : {"0", "20000"}) {
        clear_all();
        const bool forced_cap = std::string(cap) != "0";
        if (forced_cap) {
            collatz::test::set_env("COLLATZ_GPU_CPI_CAND_CAP", cap);
        }
        collatz::verify::Result r;
        if (!run_gpu(base_opts(), r)) {
            clear_all();
            return;
        }
        const auto& d = r.gpu_pipeline;
        INFO("cand cap " << std::string(cap) << " splits=" << d.cpi_split_batches
                         << " split_pipelined=" << d.split_pipelined);
        check_equal(r, cpu);
        if (forced_cap) {
            CHECK(d.cpi_split_batches > 0u);
            CHECK(d.split_pipelined > 0u);
        }
    }
    clear_all();
}

TEST_CASE("COLLATZ_GPU_MAX_ALLOC clamps the id cap, forcing candidate-cap splits") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    // B=16: 8 KiB per bitvector level, so an 80 KiB max_alloc still holds the
    // whole Step-2 table in one buffer, while the id cap drops to
    // 81920 / 4 = 20480 ids (a multiple of the 256-id grain) -- far below a
    // 2048-survivor batch's candidates, so most batches split.
    collatz::verify::Options base = base_opts();
    base.B = 16;
    const collatz::verify::Result cpu = collatz::verify::run(base);
    constexpr std::uint64_t kClampedIds = 81920u / 4u;
    // With and without a COLLATZ_GPU_CPI_CAND_CAP above the clamp (it must not
    // raise the cap past one allocation).
    for (const bool raise : {false, true}) {
        clear_all();
        collatz::test::set_env("COLLATZ_GPU_MAX_ALLOC", "81920");
        if (raise) {
            collatz::test::set_env("COLLATZ_GPU_CPI_CAND_CAP", "1000000");
        }
        collatz::verify::Result r;
        if (!run_gpu(base, r)) {
            clear_all();
            return;
        }
        INFO("cand-cap override " << raise);
        check_equal(r, cpu);
        CHECK(r.gpu_pipeline.cand_cap_ids == kClampedIds);
        CHECK(r.gpu_pipeline.cpi_split_batches > 0u);
        CHECK(r.gpu_pipeline.split_pipelined > 0u);
    }
    clear_all();
}

TEST_CASE("COLLATZ_GPU_GLOBAL_MEM lowers the id cap (bit-identical) or falls back") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    // B=16 (64 KiB of bitvectors), 2048-survivor batches, A=10. A simulated
    // device of exactly headroom (256 MiB) + fixed (256 MiB) + 2048 x 256 B +
    // 64 KiB + 2^18 ids x 4 B leaves room for 2^18 = 256 << A ids (the floor),
    // so the cap drops to 262144 and most batches split.
    collatz::verify::Options base = base_opts();
    base.B = 16;
    const collatz::verify::Result cpu = collatz::verify::run(base);
    constexpr std::uint64_t kIds = std::uint64_t{256} << 10;
    constexpr std::uint64_t kGlobal =
        (std::uint64_t{512} << 20) + 2048u * 256u + 65536u + kIds * 4u;
    clear_all();
    collatz::test::set_env("COLLATZ_GPU_GLOBAL_MEM", std::to_string(kGlobal).c_str());
    {
        collatz::verify::Result r;
        if (!run_gpu(base, r)) {
            clear_all();
            return;
        }
        check_equal(r, cpu);
        CHECK(r.gpu_pipeline.budget_cap_ids == kIds);
        CHECK(r.gpu_pipeline.cand_cap_ids == kIds);
        CHECK(r.gpu_pipeline.cpi_split_batches > 0u);
    }
    // One grain of ids below the floor: CPU fallback, still the CPU Result.
    collatz::test::set_env("COLLATZ_GPU_GLOBAL_MEM", std::to_string(kGlobal - 1024u).c_str());
    bool used = true;
    std::string msg;
    const collatz::verify::Result r = collatz::gpu::verify_gpu(base, &used, &msg);
    CHECK_FALSE(used);
    CHECK(msg.find("device budget") != std::string::npos);
    check_equal(r, cpu);
    clear_all();
}

TEST_CASE("host pipeline: self-check injections stay bit-identical") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const collatz::verify::Result cpu = collatz::verify::run(base_opts());
    for (const char* cap : {"0", "20000"}) {
        // "3": re-run on the device; "3,4": CPU fallback; copy: a lost prefetch
        // copy caught by the prefetched-input check; lost: a dropped tail launch.
        for (const char* fault : {"3", "3,4", "copy", "lost"}) {
            clear_all();
            if (std::string(cap) != "0") {
                collatz::test::set_env("COLLATZ_GPU_CPI_CAND_CAP", cap);
            }
            const std::string f = fault;
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", f == "3,4" ? "3,4" : "3");
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_COPY", f == "copy" ? "1" : "0");
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST", f == "lost" ? "1" : "0");
            collatz::verify::Result r;
            if (!run_gpu(base_opts(), r)) {
                clear_all();
                return;
            }
            INFO("fault " << f << " cand cap " << std::string(cap));
            check_equal(r, cpu);
            if (f != "copy") {
                CHECK(r.gpu_selfcheck_batches > 4);
            }
        }
    }
    clear_all();
}
