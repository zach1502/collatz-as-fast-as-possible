// Frontier block mode on the GPU. A block run of gpu::verify_gpu must equal
// verify::run on every Result field and on result_digest, for block starts from
// 2^32 to above 2^90, and through the self-check CPU fallback. Also covered:
// hellpig's 128-bit overflow start (exact wide max, overflow_fallbacks), a
// record list below 2^128, case slices merged with checkpoint::merge, and
// injected self-check faults. Reported as skipped when no OpenCL device is
// present or a run falls back to the CPU.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "gpu_block_modes.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace v = collatz::verify;
namespace ckp = collatz::checkpoint;
using collatz::u128;
using namespace collatz::test::gpu_block_modes;

TEST_CASE("gpu frontier: K=32 blocks from L=2^32 to L=2^90 equal the CPU block") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const FlushCap cap("16384");
    const u128 hs[] = {1, 12345, pow2(40) + 7, static_cast<u128>(2075) << 28, pow2(58) + 1};
    for (const u128 H : hs) {
        const v::Options o = block_opts(32, H);
        const v::Result cpu = v::run(o);
        INFO("H=" << collatz::to_string(H) << " value_bits=" << v::value_bits(o));
        CHECK(cpu.candidates_checked > 5000000);
        CHECK(cpu.preflight_candidates == 0);
        v::Result g;
        if (!run_gpu(o, g)) {
            return;
        }
        CHECK(g.gpu_selfcheck_cpu_fallback == 0);
        check_same(o, g, cpu);
    }
}

TEST_CASE("gpu frontier: K=36 block at L=2075*2^60 equals the CPU block") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    v::Options o = block_opts(36, static_cast<u128>(2075) << 24);
    o.threads = 8;
    const v::Result cpu = v::run(o);
    CHECK(v::value_bits(o) == 72u);
    v::Result g;
    if (!run_gpu(o, g)) {
        return;
    }
    check_same(o, g, cpu);
}

TEST_CASE("gpu frontier: hellpig's 128-bit overflow start gives the exact wide max") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    u128 n_hp = 0;
    REQUIRE(collatz::from_string(kHellpigN, n_hp));
    const FlushCap cap("8192");
    v::Options o = block_opts(32, n_hp >> 32);
    o.record_threshold = collatz::to_string(collatz::u128_max); // list every walk past 2^128
    const v::Result cpu = v::run(o);
    REQUIRE(cpu.overflow_fallbacks > 0);
    CHECK(cpu.max_excursion_wide == kHellpigPeak);
    CHECK(cpu.max_excursion_n == n_hp);
    REQUIRE_FALSE(cpu.records.empty());
    {
        v::Result g;
        if (!run_gpu(o, g)) {
            return;
        }
        CHECK(g.overflow_fallbacks > 0);
        CHECK(g.max_exceeds_u128);
        CHECK(g.max_excursion == collatz::u128_max);
        CHECK(g.max_excursion_wide == kHellpigPeak);
        CHECK(g.max_excursion_n == n_hp);
        check_same(o, g, cpu);
    }
    // Every batch failing its self-check: the whole block is computed by the
    // CPU fallback (process_job_cpu_exact), which folds the overflow itself.
    collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", "0,1000000");
    v::Result g;
    const bool ran = run_gpu(o, g);
    collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
    if (ran) {
        CHECK(g.gpu_selfcheck_cpu_fallback > 0);
        check_same(o, g, cpu);
    }
}

TEST_CASE("gpu frontier: a record list below 2^128 equals the CPU list") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const FlushCap cap("8192");
    v::Options o = block_opts(32, static_cast<u128>(2075) << 28);
    // A threshold a few bits below the block max, so the list is not trivial
    // and the flag bound is capped at X + 1 for most of the run.
    const v::Result base = v::run(o);
    o.record_threshold = collatz::to_string(base.max_excursion >> 3);
    const v::Result cpu = v::run(o);
    MESSAGE("record list: " << cpu.records.size() << " hits above max >> 3");
    REQUIRE(cpu.records.size() >= 1);
    v::Result g;
    if (!run_gpu(o, g)) {
        return;
    }
    check_same(o, g, cpu);
    // A cap below the list size keeps the smallest n, as on the CPU.
    o.records_cap = 1;
    const v::Result cpu1 = v::run(o);
    CHECK(cpu1.records_truncated == (cpu.records.size() > 1));
    v::Result g1;
    if (run_gpu(o, g1)) {
        check_same(o, g1, cpu1);
    }
}

TEST_CASE("gpu frontier: case slices of a GPU block merge to the unsliced block") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    u128 n_hp = 0;
    REQUIRE(collatz::from_string(kHellpigN, n_hp));
    v::Options o = block_opts(32, n_hp >> 32);
    o.record_threshold = collatz::to_string(collatz::u128_max);
    v::Result full;
    if (!run_gpu(o, full)) {
        return;
    }
    const v::Result cpu = v::run(o);
    check_same(o, full, cpu);
    ckp::JsonMeta meta;
    meta.backend = "gpu";
    meta.use_gpu = true;
    std::vector<ckp::Slice> slices;
    for (std::uint64_t c = 0; c < 4; ++c) {
        v::Options so = o;
        so.step1.case_c = c;
        so.step1.case_k = 4;
        v::Result sr;
        REQUIRE(run_gpu(so, sr));
        ckp::Slice s;
        std::string err;
        REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(meta, so, sr), s, err), err);
        slices.push_back(s);
    }
    v::Result m;
    std::string err;
    ckp::MergeInfo info;
    REQUIRE_MESSAGE(ckp::merge(slices, m, err, &info), err);
    CHECK(m.candidates_checked == full.candidates_checked);
    CHECK(m.pow_checksum == full.pow_checksum);
    CHECK(m.coverage_checksum == full.coverage_checksum);
    CHECK(m.overflow_fallbacks == full.overflow_fallbacks);
    CHECK(m.max_excursion_wide == full.max_excursion_wide);
    CHECK(m.max_excursion_n == full.max_excursion_n);
    REQUIRE(m.records.size() == full.records.size());
    CHECK(ckp::result_digest(o, info.depth, m) == digest(o, full));
}

TEST_CASE("gpu frontier: injected self-check faults are caught in block mode") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    const FlushCap cap("512");
    const v::Options o = block_opts(32, static_cast<u128>(2075) << 28);
    const v::Result cpu = v::run(o);
    for (const char* lost : {"0", "1"}) {
        collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST", lost);
        // "3": one corrupted dispatch -> detected and re-run on the device.
        // "3,4": the batch and its 3 re-runs corrupted -> computed on the CPU.
        for (const char* inject : {"3", "3,4"}) {
            INFO("lost " << lost << " inject " << inject);
            collatz::test::set_env("COLLATZ_GPU_SELFCHECK_INJECT", inject);
            v::Result g;
            const bool ran = run_gpu(o, g);
            collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT");
            if (!ran) {
                collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST");
                return;
            }
            REQUIRE(g.gpu_selfcheck_on);
            CHECK(g.gpu_selfcheck_batches > 4);
            if (std::string(inject) == "3") {
                CHECK(g.gpu_selfcheck_retried >= 1);
            } else {
                CHECK(g.gpu_selfcheck_retried >= 3);
                CHECK(g.gpu_selfcheck_cpu_fallback >= 1);
            }
            check_same(o, g, cpu);
        }
    }
    collatz::test::unset_env("COLLATZ_GPU_SELFCHECK_INJECT_LOST");
}
