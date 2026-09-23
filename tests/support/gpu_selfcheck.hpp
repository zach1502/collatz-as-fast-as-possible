#pragma once

// Helpers shared by the files of test_integration_gpu_selfcheck
// (tests/integration/test_gpu_selfcheck.cpp and test_gpu_selfcheck/): the N=30
// options every case runs and the comparison of a GPU Result with the CPU
// Result.
#include "collatz/progress.hpp"
#include "collatz/verify.hpp"

#include <doctest/doctest.h>

namespace collatz::test::selfcheck {

inline collatz::verify::Options base_opts() {
    collatz::verify::Options o;
    o.N = 30;
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
    // The run just checked counted each survivor finished once (the
    // --progress-file counts), through every re-run, split and fallback.
    const collatz::progress::SurvivorCounts c = collatz::progress::survivor_counts();
    CHECK(c.found > 0);
    CHECK(c.done == c.found);
}

} // namespace collatz::test::selfcheck
