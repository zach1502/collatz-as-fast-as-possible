#pragma once

// Helpers shared by the checkpoint unit tests: the options of a small A = 0 run
// and a check that two Results agree on every mergeable field.
#include "collatz/verify.hpp"

#include <doctest/doctest.h>

namespace collatz::test {

inline collatz::verify::Options small_opts(unsigned N) {
    // A = 0: no Step-2 filter, so no bitvector cache dependency; the slicing
    // and the merge are what is under test.
    collatz::verify::Options o;
    o.N = N;
    o.A = 0;
    o.threads = 2;
    o.step1.mod3t = 6;
    return o;
}

inline void check_same(const collatz::verify::Result& a, const collatz::verify::Result& b) {
    CHECK(a.candidates_checked == b.candidates_checked);
    CHECK(a.step2_masked_out == b.step2_masked_out);
    CHECK(a.step3_steps_total == b.step3_steps_total);
    CHECK(a.coverage_checksum == b.coverage_checksum);
    CHECK(a.pow_checksum == b.pow_checksum);
    CHECK(a.blocks_total == b.blocks_total);
    CHECK(a.pow_available == b.pow_available);
    CHECK(a.pow_kernel_id == b.pow_kernel_id);
    CHECK(a.max_excursion == b.max_excursion);
    CHECK(a.max_excursion_n == b.max_excursion_n);
    CHECK(a.max_exceeds_u128 == b.max_exceeds_u128);
    CHECK(a.max_exact == b.max_exact);
    CHECK(a.overflow_fallbacks == b.overflow_fallbacks);
    CHECK(a.step1.survivors == b.step1.survivors);
}

} // namespace collatz::test
