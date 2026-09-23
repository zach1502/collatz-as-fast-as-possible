#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// The fixed tail_persistent build options and grid (src/gpu/tail_defaults.hpp).
// Header-only, so it runs without an OpenCL device.

#include "tail_defaults.hpp"

#include <doctest/doctest.h>

using collatz::gpu::TailHint;

TEST_CASE("tail defaults: U64N is on up to 64-bit values") {
    CHECK(collatz::gpu::tail_u64n_on(TailHint{40}));
    CHECK(collatz::gpu::tail_u64n_on(TailHint{64}));
    CHECK_FALSE(collatz::gpu::tail_u64n_on(TailHint{65}));
    CHECK_FALSE(collatz::gpu::tail_u64n_on(TailHint{96}));
}

TEST_CASE("tail defaults: the build options and the grid") {
    CHECK(collatz::gpu::tail_build_defines(true) == " -DCOLLATZ_GPU_TAIL_U64N=1");
    CHECK(collatz::gpu::tail_build_defines(false) == " -DCOLLATZ_GPU_TAIL_U64N=0");
    CHECK(collatz::gpu::kPersistMult == 24u);
    CHECK(collatz::gpu::kPersistChunk == 16u);
}
