#pragma once

// The GPU block runs of tests/integration/test_gpu_frontier_blocks.cpp and
// test_gpu_wide_a.cpp: the CLI's block options below K = 40, run_gpu, and
// check_same, which compares every Result field and the result digest with the
// CPU run.
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "gpu_present.hpp"      // gpu_ran
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

namespace collatz::test::gpu_block_modes {

namespace v = collatz::verify;
namespace ckp = collatz::checkpoint;
using collatz::u128;

constexpr const char* kHellpigN = "274133054632352106267"; // climbs to about 2^135.4
constexpr const char* kHellpigPeak = "56649062372194325899121269007146717645316";

inline u128 pow2(unsigned k) {
    return static_cast<u128>(1) << k;
}

/// The CLI's block configuration below K = 40 (A = 10, B = 24, the default sieves).
inline v::Options block_opts(unsigned K, u128 H) {
    v::Options o;
    o.N = K;
    o.block_h = H;
    o.A = 10;
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = 4;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.bv_cache = collatz::test::scoped_bv_cache();
    return o;
}

/// Runs the GPU path; false, with a recorded skip, when it fell back to the CPU.
inline bool run_gpu(const v::Options& o, v::Result& out) {
    bool used = false;
    std::string msg;
    out = collatz::gpu::verify_gpu(o, &used, &msg);
    return collatz::test::gpu_ran(used, msg);
}

inline std::string digest(const v::Options& o, const v::Result& r) {
    return ckp::result_digest(o, v::unwalked_depth(o), r);
}

/// Every Result field that a work unit records, and the digest over them.
inline void check_same(const v::Options& o, const v::Result& g, const v::Result& c) {
    CHECK(g.candidates_checked == c.candidates_checked);
    CHECK(g.step2_masked_out == c.step2_masked_out);
    CHECK(g.step3_steps_total == c.step3_steps_total);
    CHECK(g.coverage_checksum == c.coverage_checksum);
    CHECK(g.pow_checksum == c.pow_checksum);
    CHECK(g.blocks_total == c.blocks_total);
    CHECK(g.pow_available == c.pow_available);
    CHECK(g.pow_kernel_id == c.pow_kernel_id);
    CHECK(g.max_excursion == c.max_excursion);
    CHECK(g.max_excursion_n == c.max_excursion_n);
    CHECK(g.max_exceeds_u128 == c.max_exceeds_u128);
    CHECK(g.max_excursion_wide == c.max_excursion_wide);
    CHECK(g.max_exact == c.max_exact);
    CHECK(g.overflow_fallbacks == c.overflow_fallbacks);
    CHECK(g.records_truncated == c.records_truncated);
    REQUIRE(g.records.size() == c.records.size());
    for (std::size_t i = 0; i < g.records.size(); ++i) {
        CHECK(g.records[i].n == c.records[i].n);
        CHECK(g.records[i].peak == c.records[i].peak);
    }
    CHECK(g.step1.survivors == c.step1.survivors);
    CHECK(g.preflight_candidates == c.preflight_candidates);
    CHECK(g.preflight_coverage == c.preflight_coverage);
    CHECK(g.preflight_pow == c.preflight_pow);
    CHECK(g.preflight_blocks == c.preflight_blocks);
    CHECK(g.preflight_step3_steps == c.preflight_step3_steps);
    CHECK(digest(o, g) == digest(o, c));
}

/// Several batches per run, so the batch boundaries differ from a single batch.
struct FlushCap {
    explicit FlushCap(const char* cap) {
        collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", cap);
    }
    ~FlushCap() {
        collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP");
    }
    FlushCap(const FlushCap&) = delete;
    FlushCap& operator=(const FlushCap&) = delete;
};

} // namespace collatz::test::gpu_block_modes
