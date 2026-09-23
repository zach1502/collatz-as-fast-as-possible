// How verify_gpu ends when the device path fails. A failure after setup, on
// the dispatch thread (a failed OpenCL enqueue) or on a producer thread, must
// reach the caller as an exception, which `collatz verify` reports as an error
// with rc 2, and must not call std::terminate (rc 134). The test hooks
// COLLATZ_GPU_INJECT_CL_FAIL and COLLATZ_GPU_INJECT_PRODUCER_FAIL raise those
// failures, and the run after a failed one must still equal the CPU. A device
// that fails during setup is not used: a split tail that does not fit the local
// memory (COLLATZ_GPU_LOCAL_MEM lowers it) and a COLLATZ_GPU_DEVICE that names
// no GPU both give the CPU result with used_gpu = false. The device list that
// `collatz gpu-info` prints has the GPUs first and marks the one a run tries
// first.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/verify.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <cstddef>
#include <doctest/doctest.h>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

namespace v = collatz::verify;

// The runner owns raw OpenCL handles and is handed out by pointer, so it has no
// move: a hand-written move must list every member and silently drops one added
// later.
static_assert(!std::is_move_constructible_v<collatz::gpu::SurvivorRunner>);
static_assert(!std::is_move_assignable_v<collatz::gpu::SurvivorRunner>);

namespace {

v::Options opts(unsigned threads) {
    v::Options o;
    o.N = 30;
    o.A = 10; // M = 20, above the Step-1 split depth, so threads > 1 run workers
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = threads;
    o.window_chains = 6;
    return o;
}

void check_same(const v::Result& g, const v::Result& cpu) {
    CHECK(g.candidates_checked == cpu.candidates_checked);
    CHECK(g.step2_masked_out == cpu.step2_masked_out);
    CHECK(g.blocks_total == cpu.blocks_total);
    CHECK(g.coverage_checksum == cpu.coverage_checksum);
    CHECK(g.pow_checksum == cpu.pow_checksum);
    CHECK(g.max_excursion == cpu.max_excursion);
    CHECK(g.max_excursion_n == cpu.max_excursion_n);
}

// The hooks off, and batches of 512 survivors, so an N=30 run dispatches
// dozens of batches and the in-flight pipeline holds two of them.
void reset_env() {
    for (const char* e : {"COLLATZ_GPU_INJECT_CL_FAIL",
                          "COLLATZ_GPU_INJECT_PRODUCER_FAIL",
                          "COLLATZ_GPU_LOCAL_MEM",
                          "COLLATZ_GPU_DEVICE"}) {
        collatz::test::unset_env(e);
    }
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "512");
}

// The message of the exception verify_gpu ends with; empty when it returns.
std::string run_error(const v::Options& o) {
    bool used = false;
    std::string msg;
    try {
        (void) collatz::gpu::verify_gpu(o, &used, &msg);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

// A clean run on the device, compared with `cpu`; false (a recorded skip) when
// the GPU path fell back.
bool clean_gpu_run(const v::Options& o, const v::Result& cpu) {
    bool used = false;
    std::string msg;
    const v::Result g = collatz::gpu::verify_gpu(o, &used, &msg);
    if (!collatz::test::gpu_ran(used, msg)) {
        return false;
    }
    check_same(g, cpu);
    return true;
}

} // namespace

TEST_CASE("gpu errors: a failed tail launch ends the run with an exception") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    reset_env();
    const v::Result cpu = v::run(opts(2));
    if (!clean_gpu_run(opts(2), cpu)) {
        return;
    }
    // Launch 1 fails before any batch is folded; launch 5 with batches in flight.
    for (const char* k : {"1", "5"}) {
        INFO("COLLATZ_GPU_INJECT_CL_FAIL=" << k);
        collatz::test::set_env("COLLATZ_GPU_INJECT_CL_FAIL", k);
        const std::string what = run_error(opts(2));
        CHECK(what.find("COLLATZ_GPU_INJECT_CL_FAIL") != std::string::npos);
        CHECK(what.find("CL_OUT_OF_RESOURCES") != std::string::npos);
    }
    reset_env();
    clean_gpu_run(opts(2), cpu);
}

TEST_CASE("gpu errors: a failed producer ends the run with an exception") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    reset_env();
    const v::Result cpu = v::run(opts(2));
    if (!clean_gpu_run(opts(2), cpu)) {
        return;
    }
    // One thread runs Step 1 on the calling thread; two run it on workers.
    for (const unsigned threads : {1u, 2u}) {
        INFO("threads=" << threads);
        collatz::test::set_env("COLLATZ_GPU_INJECT_PRODUCER_FAIL", "3");
        const std::string what = run_error(opts(threads));
        CHECK(what.find("COLLATZ_GPU_INJECT_PRODUCER_FAIL") != std::string::npos);
    }
    reset_env();
    clean_gpu_run(opts(2), cpu);
}

TEST_CASE("gpu errors: a split tail above the local memory falls back to the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    reset_env();
    const v::Result cpu = v::run(opts(2));
    collatz::test::set_env("COLLATZ_GPU_LOCAL_MEM", "1024");
    bool used = true;
    std::string msg;
    v::Result g;
    CHECK_NOTHROW(g = collatz::gpu::verify_gpu(opts(2), &used, &msg));
    CHECK_FALSE(used);
    CHECK(msg.find("split tail") != std::string::npos);
    check_same(g, cpu);
    reset_env();
}

TEST_CASE("gpu errors: a COLLATZ_GPU_DEVICE that names no GPU falls back to the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    reset_env();
    const v::Result cpu = v::run(opts(2));
    for (const char* bad : {"999", "x"}) {
        INFO("COLLATZ_GPU_DEVICE=" << bad);
        collatz::test::set_env("COLLATZ_GPU_DEVICE", bad);
        bool used = true;
        std::string msg;
        const v::Result g = collatz::gpu::verify_gpu(opts(2), &used, &msg);
        CHECK_FALSE(used);
        CHECK(msg.find("COLLATZ_GPU_DEVICE") != std::string::npos);
        check_same(g, cpu);
    }
    // Index 0 is the device the run picks without the variable.
    collatz::test::set_env("COLLATZ_GPU_DEVICE", "0");
    clean_gpu_run(opts(2), cpu);
    reset_env();
}

TEST_CASE("gpu device list: the GPUs first, the first one selected, COLLATZ_GPU_DEVICE moves it") {
    reset_env();
    const std::vector<collatz::gpu::DeviceInfo> devs = collatz::gpu::enumerate_devices();
    std::size_t gpus = 0;
    while (gpus < devs.size() && devs[gpus].candidate) {
        ++gpus;
    }
    std::size_t selected = 0;
    for (std::size_t i = 0; i < devs.size(); ++i) {
        const collatz::gpu::DeviceInfo& d = devs[i];
        CHECK(d.candidate == (d.available && (d.device_type & CL_DEVICE_TYPE_GPU) != 0));
        CHECK((i < gpus || !d.candidate)); // the candidates are a prefix
        selected += d.selected ? 1u : 0u;
    }
    CHECK(selected == (gpus > 0 ? 1u : 0u));
    CHECK(collatz::gpu::gpu_choice().tries.size() == gpus);
    if (gpus > 0) {
        CHECK(devs[0].selected);
        CHECK(collatz::gpu::selected_device_name() == devs[0].device_name);
    }
    collatz::test::set_env("COLLATZ_GPU_DEVICE", "999");
    for (const collatz::gpu::DeviceInfo& d : collatz::gpu::enumerate_devices()) {
        CHECK_FALSE(d.selected);
    }
    CHECK(collatz::gpu::gpu_choice().tries.empty());
    CHECK(collatz::gpu::gpu_choice().why.find("COLLATZ_GPU_DEVICE=999") != std::string::npos);
    CHECK_FALSE(collatz::gpu::selected_device_name().has_value());
    if (gpus > 1) {
        collatz::test::set_env("COLLATZ_GPU_DEVICE", "1");
        const std::vector<collatz::gpu::DeviceInfo> moved = collatz::gpu::enumerate_devices();
        CHECK_FALSE(moved[0].selected);
        CHECK(moved[1].selected);
        CHECK(collatz::gpu::gpu_choice().tries.size() == 1u);
    }
    reset_env();
}
