// The temp-file sweep that resolve_dir runs (src/core/step2_cache_sweep.cpp): a
// cache writer's <file>.tmp.<pid> is deleted when that process is gone and the
// file has not changed for 10 minutes; every other file stays.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step2_cache.hpp"
#include "step2_cache_dir.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("resolve_dir deletes the temp files of dead writers only") {
    const std::string dir = collatz::test::fresh_cache_dir("sweep");
    const auto make = [&dir](const std::string& name, bool old) {
        const fs::path p = fs::path(dir) / name;
        std::ofstream(p) << "x";
        if (old) {
            fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(1));
        }
        return p;
    };
    // Windows pids are multiples of 4, so no process has 99999999.
    const std::string dead = ".tmp.99999999";
    const fs::path dip = make("dip_B20_tw6_v4.u16" + dead, true);
    const fs::path bv = make("bv_B20_M22_k8_tw6_v4.bin" + dead, true);
    const fs::path recent = make("dip_B21_tw6_v4.u16" + dead, false);
    const fs::path own =
        make("dip_B22_tw6_v4.u16.tmp." + std::to_string(collatz::test::test_pid()), true);
    const fs::path other = make("notes.txt" + dead, true);
    const fs::path table = make("dip_B20_tw6_v4.u16", true);

    REQUIRE(collatz::step2::cache::resolve_dir() == dir);
    CHECK_FALSE(fs::exists(dip));
    CHECK_FALSE(fs::exists(bv));
    CHECK(fs::exists(recent));
    CHECK(fs::exists(own));
    CHECK(fs::exists(other));
    CHECK(fs::exists(table));
}
