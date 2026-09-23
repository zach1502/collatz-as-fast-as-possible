// Filter with the on-disk Step-2 cache (collatz/step2_cache.hpp): a cache hit is
// bit-identical to a fresh build, and cache_only refuses a miss and loads a hit.
// Each test uses a per-process temp cache dir and small B.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step2.hpp"
#include "step2_cache_dir.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

using collatz::test::unique_tmp_dir;

namespace {

// bitvectors() yields std::span (a view into owned storage or the
// cache mmap), which has no operator==; compare element-wise.
[[nodiscard]] bool bv_equal(std::span<const std::uint64_t> a, std::span<const std::uint64_t> b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Filter with cache is bit-identical to without") {
    unique_tmp_dir();

    collatz::step2::Params base;
    base.N = 28;
    base.A = 10;
    base.B = 16; // small, fast
    base.num_bitvectors = 8;
    base.threads = 2;

    collatz::step2::Params off = base;
    off.use_bv_cache = false;
    const collatz::step2::Filter f_off(off);

    collatz::step2::Params on = base;
    on.use_bv_cache = true;
    const collatz::step2::Filter f_miss(on); // builds + writes the cache
    const collatz::step2::Filter f_hit(on);  // loads the cache

    CHECK_FALSE(f_miss.bv_cache_hit());
    CHECK(f_hit.bv_cache_hit());

    REQUIRE(f_off.bitvectors().size() == f_hit.bitvectors().size());
    for (unsigned i = 0; i < base.num_bitvectors; ++i) {
        CHECK(bv_equal(f_off.bitvectors()[i], f_miss.bitvectors()[i]));
        CHECK(bv_equal(f_off.bitvectors()[i], f_hit.bitvectors()[i]));
        CHECK(f_off.inv3f(i) == f_hit.inv3f(i));
    }
}

TEST_CASE("cache_only refuses a miss and loads a hit") {
    const std::string dir = unique_tmp_dir();

    collatz::step2::Params base;
    base.N = 30;
    base.A = 10;
    base.B = 15; // small and used by no other test here, so the cache starts empty
    base.num_bitvectors = 8;
    base.threads = 2;
    base.use_bv_cache = true;

    const std::filesystem::path bv_file = std::filesystem::path(dir) / "bv_B15_M20_k8_tw0_v4.bin";
    std::error_code ec;
    std::filesystem::remove(bv_file, ec);

    // Nothing cached yet: a cache-only filter refuses to build.
    collatz::step2::Params only = base;
    only.cache_only = true;
    CHECK_THROWS_AS(collatz::step2::Filter{only}, std::runtime_error);

    const collatz::step2::Filter miss(base);
    CHECK_FALSE(miss.bv_cache_hit());
    CHECK(std::filesystem::exists(bv_file));

    const collatz::step2::Filter hit(only); // now cached, so cache_only loads it
    CHECK(hit.bv_cache_hit());
    for (unsigned i = 0; i < base.num_bitvectors; ++i) {
        CHECK(bv_equal(miss.bitvectors()[i], hit.bitvectors()[i]));
    }

    // Without the cache there is nothing to load either.
    collatz::step2::Params off = only;
    off.use_bv_cache = false;
    CHECK_THROWS_AS(collatz::step2::Filter{off}, std::runtime_error);
}
