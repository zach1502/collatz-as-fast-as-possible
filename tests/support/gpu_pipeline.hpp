#pragma once

// The --gpu host pipeline runs shared by tests/integration/test_gpu_host_pipeline.cpp
// and test_gpu_host_pipeline_pack.cpp: the base options, the equality check with
// the CPU Result (and the survivor counts), run_gpu and clear_all.
#include "collatz/progress.hpp"
#include "collatz/verify.hpp"
#include "gpu_present.hpp"      // gpu_ran
#include "survivor_runtime.hpp" // collatz::gpu::verify_gpu
#include "test_env.hpp"

#include <doctest/doctest.h>
#include <string>

namespace collatz::test::gpu_pipeline {

inline collatz::verify::Options base_opts() {
    collatz::verify::Options o;
    o.N = 32;
    o.A = 10;
    o.B = 24;
    o.num_bitvectors = 8;
    o.threads = 4;
    o.window_chains = 6;
    return o;
}

inline void check_equal(const collatz::verify::Result& r, const collatz::verify::Result& cpu) {
    CHECK(r.candidates_checked == cpu.candidates_checked);
    CHECK(r.step2_masked_out == cpu.step2_masked_out);
    CHECK(r.blocks_total == cpu.blocks_total);
    CHECK(r.coverage_checksum == cpu.coverage_checksum);
    CHECK(r.pow_checksum == cpu.pow_checksum);
    CHECK(r.max_excursion == cpu.max_excursion);
    CHECK(r.max_excursion_n == cpu.max_excursion_n);
    // The GPU path must fill the preflight_* fields, which a slice merge uses to
    // count the preflight once; left at zero, a merge of GPU slices counts it
    // once per slice.
    CHECK(r.preflight_candidates == cpu.preflight_candidates);
    CHECK(r.preflight_coverage == cpu.preflight_coverage);
    CHECK(r.preflight_pow == cpu.preflight_pow);
    CHECK(r.preflight_blocks == cpu.preflight_blocks);
    CHECK(r.preflight_step3_steps == cpu.preflight_step3_steps);
    // The run just checked counted each survivor finished once (the
    // --progress-file counts): in-flight batches, split halves, re-runs.
    const collatz::progress::SurvivorCounts c = collatz::progress::survivor_counts();
    CHECK(c.found > 0);
    CHECK(c.done == c.found);
}

/// Runs the GPU path; false, with a recorded skip, when it fell back to the CPU.
inline bool run_gpu(const collatz::verify::Options& o, collatz::verify::Result& out) {
    bool used = false;
    std::string msg;
    out = collatz::gpu::verify_gpu(o, &used, &msg);
    return collatz::test::gpu_ran(used, msg);
}

/// Clears every env knob these tests set, and sets 2048-survivor batches so a
/// tiny N still runs many batches.
inline void clear_all() {
    for (const char* e : {"COLLATZ_GPU_CPI_CAND_CAP",
                          "COLLATZ_GPU_MAX_ALLOC",
                          "COLLATZ_GPU_GLOBAL_MEM",
                          "COLLATZ_GPU_SELFCHECK_INJECT",
                          "COLLATZ_GPU_SELFCHECK_INJECT_COPY",
                          "COLLATZ_GPU_SELFCHECK_INJECT_LOST",
                          "COLLATZ_GPU_EARLY_UPLOAD_INJECT_FAIL",
                          "COLLATZ_GPU_EARLY_UPLOAD_CHUNK_MB"}) {
        collatz::test::unset_env(e);
    }
    collatz::test::set_env("COLLATZ_GPU_FLUSH_CAP", "2048");
}

} // namespace collatz::test::gpu_pipeline
