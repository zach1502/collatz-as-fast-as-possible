#pragma once

// main() of a GPU test executable. Its one source file that defines
// DOCTEST_CONFIG_IMPLEMENT (before any include) includes this header instead
// of using DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN. It runs doctest as that main
// does, and when no case failed but one recorded a skip (gpu_present.hpp) it
// exits with kGpuSkipCode, so CTest reports the entry as skipped.
#include "gpu_present.hpp"

#include <doctest/doctest.h>

int main(int argc, char** argv) {
    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    if (ctx.shouldExit() || rc != 0) {
        return rc;
    }
    return collatz::test::gpu_skip_count() != 0 ? collatz::test::kGpuSkipCode : 0;
}
