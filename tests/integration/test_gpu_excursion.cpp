// The excursion the GPU path records (src/gpu/verify_gpu*.cpp) against the CPU
// verifier and its definition (excursion_dec): random small blocks with the
// record list off, at "0" and above a listed excursion, and a full run whose
// threshold lists numbers of the preflight. Reported as skipped when no OpenCL
// device is present or a run falls back to the CPU.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "frontier_model/random_block.hpp"
#include "frontier_model/walk.hpp"
#include "gpu_block_modes.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "scale.hpp"

#include <doctest/doctest.h>
#include <random>
#include <string>
#include <vector>

namespace v = collatz::verify;
using collatz::u128;
using collatz::frontier_test::excursion_dec;
using namespace collatz::test::gpu_block_modes;

TEST_CASE("gpu excursion: random small blocks equal the CPU and the definition") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    std::mt19937_64 g(0x6b0c4e1dULL);
    // K in [8, 14], A >= 5 and B >= 6: the device path's limits.
    const collatz::frontier_test::BlockDraw d{8, 14, 5, 6};
    const unsigned cases = collatz::test::scaled(120, 12);
    for (unsigned c = 0; c < cases; ++c) {
        v::Options o = collatz::frontier_test::random_block(g, d);
        CAPTURE(c);
        CAPTURE(collatz::to_string(o.block_h));
        CAPTURE(o.N);
        CAPTURE(o.A);
        CAPTURE(o.B);
        o.record_threshold = "0";
        const v::Result all = v::run(o);
        REQUIRE(all.records.size() == all.candidates_checked);
        for (const v::RecordHit& h : all.records) {
            CHECK(h.peak == excursion_dec(h.n));
        }
        std::vector<std::string> thresholds = {"", "0"};
        if (!all.records.empty()) {
            thresholds.push_back(all.records[g() % all.records.size()].peak);
        }
        for (const std::string& t : thresholds) {
            CAPTURE(t);
            o.record_threshold = t;
            const v::Result cpu = t == "0" ? all : v::run(o);
            v::Result gpu;
            if (!run_gpu(o, gpu)) {
                return;
            }
            check_same(o, gpu, cpu);
        }
    }
}

TEST_CASE("gpu excursion: a full run lists the preflight numbers above the threshold") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    // The CLI run `verify --N 18 --record-threshold 700000000 --threads 1`.
    v::Options o = block_opts(18, 0);
    o.threads = 1;
    o.record_threshold = "700000000";
    const v::Result cpu = v::run(o);
    REQUIRE_FALSE(cpu.records.empty());
    CHECK(cpu.records.front().n < (static_cast<u128>(1) << 17)); // from the preflight
    for (const v::RecordHit& h : cpu.records) {
        CHECK(h.peak == excursion_dec(h.n));
    }
    v::Result g;
    if (!run_gpu(o, g)) {
        return;
    }
    check_same(o, g, cpu);
}
