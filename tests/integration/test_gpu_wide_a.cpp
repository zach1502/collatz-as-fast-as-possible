// GPU against CPU at A=13 and A=14, where the batch cap of the --gpu path
// matters. The device mask packs a candidate as (s << A) | a in 32 bits, so a
// batch may hold at most 2^(32-A) survivors: 2^19 at A=13 and 2^18 at A=14.
// With one Step-1 thread every survivor goes to one producer, and with the
// default 2^20 flush cap (COLLATZ_GPU_FLUSH_CAP unset) its first batch holds
// 2^17 survivors and its last all the rest. At Step-1 depth M = 26 that is
// more than 2^(32-A), so before the cap the ids of that batch wrapped: some
// candidates were walked twice and others never, and every self-check passed.
// Each case compares every Result field and the result digest with the CPU,
// for a full run and for the frontier block from 2075*2^60. A device whose
// mask work-groups cannot hold 2^A/32 work-items
// falls back to the CPU, and the case is reported as skipped.

#define DOCTEST_CONFIG_IMPLEMENT

#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "gpu_block_modes.hpp"
#include "gpu_present.hpp"
#include "gpu_test_main.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <doctest/doctest.h>

namespace v = collatz::verify;
using collatz::u128;
using namespace collatz::test::gpu_block_modes;

namespace {

constexpr unsigned kDepthM = 26;

// Runs block H at K = 26 + A (H = 0: the full run below 2^K) on the GPU with
// one Step-1 thread and on the CPU with every thread, and compares them.
// Returns false when the GPU path fell back (a recorded skip).
bool check_wide_a(unsigned A, u128 H) {
    collatz::test::unset_env("COLLATZ_GPU_FLUSH_CAP"); // the real 2^20 cap
    v::Options o = block_opts(kDepthM + A, H);
    o.A = A;
    o.threads = 1;
    INFO("A=" << A << " H=" << collatz::to_string(H));
    v::Result g;
    if (!run_gpu(o, g)) {
        return false;
    }
    v::Options oc = o;
    oc.threads = 0;
    const v::Result cpu = v::run(oc);
    // Without the cap the last batch held every survivor past the first 2^17,
    // which is more than 2^(32-A) only if this holds.
    REQUIRE(cpu.step1.survivors > (std::uint64_t{1} << 17) + (std::uint64_t{1} << (32u - A)));
    CHECK(g.gpu_selfcheck_cpu_fallback == 0);
    check_same(o, g, cpu);
    return true;
}

} // namespace

TEST_CASE("gpu wide A: A=13 full run and frontier block equal the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    if (check_wide_a(13, 0)) {
        check_wide_a(13, static_cast<u128>(2075) << 21); // [2075*2^60, +2^39)
    }
}

TEST_CASE("gpu wide A: A=14 full run and frontier block equal the CPU") {
    if (collatz::test::skip_without_gpu()) {
        return;
    }
    if (check_wide_a(14, 0)) {
        check_wide_a(14, static_cast<u128>(2075) << 20); // [2075*2^60, +2^40)
    }
}
