// Step 2 class-major cold build (src/core/step2_coldbv.cpp and
// step2_coldbv_walk.cpp): its bitvectors and cache files against the
// per-residue definition, built here from Filter::dip.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "collatz/u128.hpp"
#include "test_env.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// _getpid() for the per-run temp cache dir (see tests/support/step2_cache_dir.hpp).
#include <process.h>

namespace cold_build {

// At B >= 14 the Filter builds its bitvectors with the class-major cold build: it
// stores the dip table as cm[(r mod 2^L) << H | r >> L], walks the last H levels
// as a binary tree, packs in tiles, and streams the dip and bitvector cache
// files. None of that may show in the output. These tests pin the packed
// bitvectors, inv3f and both cache files against the definition (bit j of BV_i
// is set iff dip(j * 3^{fmin+i} mod 2^B) < 485 (fmin + i) - 306 M) for several
// (B, N, window terms), including the dip-cache hit that reads a natural-order
// dip file into the class-major table.

namespace {

namespace cache = collatz::step2::cache;
using collatz::step2::Filter;
using collatz::step2::Params;
using Bitvectors = std::vector<std::vector<std::uint64_t>>;

constexpr unsigned kBitvectors = 8;

struct Cfg {
    unsigned N, A, B, tw, D;
};

unsigned fmin_of(const Cfg& c) {
    return (306u * (c.N - c.A) + 485u) / 485u; // ceil((306 M + 1) / 485)
}

Params params(const Cfg& c, bool use_cache) {
    Params p;
    p.N = c.N;
    p.A = c.A;
    p.B = c.B;
    p.num_bitvectors = kBitvectors;
    p.window_chains = c.tw;
    p.window_templates_D = c.D;
    p.threads = 4;
    p.use_bv_cache = use_cache;
    return p;
}

// The natural-order dip table from the per-residue Filter::dip, with the window
// tables a Filter builds for (tw, D).
std::vector<std::uint16_t> dip_table(const Cfg& c) {
    using collatz::preimage::WindowChainTable;
    std::unique_ptr<WindowChainTable> wct;
    std::unique_ptr<WindowChainTable> wtt;
    if (c.tw != 0) {
        wct = std::make_unique<WindowChainTable>(c.tw);
    }
    if (c.D != 0) {
        const unsigned t = std::min(c.D, collatz::preimage::max_window_t);
        wtt = std::make_unique<WindowChainTable>(t, c.D);
    }
    std::vector<std::uint16_t> dip(std::size_t{1} << c.B);
    for (std::uint64_t r = 0; r < dip.size(); ++r) {
        dip[r] = static_cast<std::uint16_t>(Filter::dip(r, c.B, wct.get(), wtt.get()));
    }
    return dip;
}

// The bitvectors by definition from `dip`.
Bitvectors reference(const Cfg& c, const std::vector<std::uint16_t>& dip) {
    const unsigned M = c.N - c.A;
    const std::uint64_t mask = (std::uint64_t{1} << c.B) - 1u;
    Bitvectors out(kBitvectors, std::vector<std::uint64_t>((mask + 1u) / 64u, 0));
    for (unsigned i = 0; i < kBitvectors; ++i) {
        const unsigned f = fmin_of(c) + i;
        const std::int64_t t = 485LL * f - 306LL * M;
        const std::uint64_t step = collatz::lo64(collatz::pow3(f)) & mask;
        std::uint64_t r = 0;
        for (std::uint64_t& w : out[i]) {
            for (unsigned b = 0; b < 64u; ++b) {
                w |= static_cast<std::uint64_t>(static_cast<std::int64_t>(dip[r]) < t) << b;
                r = (r + step) & mask;
            }
        }
    }
    return out;
}

void check_filter(const Filter& f, const Cfg& c, const Bitvectors& ref) {
    CHECK(f.fmin() == fmin_of(c));
    REQUIRE(f.bitvectors().size() == ref.size());
    for (unsigned i = 0; i < ref.size(); ++i) {
        CAPTURE(i);
        CHECK(f.inv3f(i) == Filter::inverse_pow3_mod2b(fmin_of(c) + i, c.B));
        const auto bv = f.bitvectors()[i];
        CHECK(std::equal(bv.begin(), bv.end(), ref[i].begin(), ref[i].end()));
    }
}

// The bitvector cache file of c holds inv3f, the set-bit count and the words of
// every reference bitvector.
void check_bv_file(const std::string& dir, const Cfg& c, const Bitvectors& ref) {
    const cache::BvKey key{c.B, c.N - c.A, kBitvectors, fmin_of(c), c.tw + 32u * c.D};
    const cache::MappedBitvectors m = cache::map_bitvectors(dir, key);
    REQUIRE(m.ok);
    REQUIRE(m.bv.size() == ref.size());
    REQUIRE(m.words == ref[0].size());
    for (unsigned i = 0; i < ref.size(); ++i) {
        CAPTURE(i);
        std::uint64_t set = 0;
        for (const std::uint64_t w : ref[i]) {
            set += static_cast<std::uint64_t>(std::popcount(w));
        }
        CHECK(m.popcount[i] == set);
        CHECK(m.inv3f[i] == Filter::inverse_pow3_mod2b(fmin_of(c) + i, c.B));
        CHECK(std::equal(ref[i].begin(), ref[i].end(), m.bv[i]));
    }
}

std::string temp_dir() {
    const auto pid = _getpid();
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("collatz_cold_bv_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}

} // namespace

TEST_CASE("cold build: bitvectors equal the per-residue definition") {
#ifdef NDEBUG
    const unsigned kBig = 20;
    const unsigned kD = 12; // shipped --window-templates-D.
#else
    // debug + ASan: the D=12 template table dominates each build, so keep one
    // D=12 config and small B; release covers B=20 with D=12.
    const unsigned kBig = 17;
    const unsigned kD = 0;
#endif
    for (const Cfg c : {Cfg{30, 10, 14, 6, 12},
                        Cfg{40, 12, 16, 0, 0},
                        Cfg{33, 9, 16, 6, 0},
                        Cfg{36, 10, kBig, 6, kD},
                        Cfg{34, 10, kBig, 3, 0}}) {
        CAPTURE(c.N);
        CAPTURE(c.B);
        CAPTURE(c.tw);
        CAPTURE(c.D);
        const Filter f(params(c, false));
        check_filter(f, c, reference(c, dip_table(c)));
    }
}

TEST_CASE("cold build: the cache files hold the definition, and a dip-cache hit packs the file") {
    const std::string dir = temp_dir();
    collatz::test::set_env("COLLATZ_CACHE_DIR", dir.c_str());
#ifdef NDEBUG
    const unsigned kD = 12; // shipped --window-templates-D.
#else
    const unsigned kD = 0; // debug + ASan: skip the slow D=12 table build.
#endif
    const Cfg a{34, 10, 18, 6, kD};
    const Cfg b{36, 10, 18, 6, kD}; // same B and window terms: a dip-cache hit
    const unsigned tag = a.tw + 32u * a.D;
    const std::vector<std::uint16_t> dip = dip_table(a);
    {
        const Filter f(params(a, true)); // both cache files miss
        CHECK_FALSE(f.bv_cache_hit());
        const Bitvectors ref = reference(a, dip);
        check_filter(f, a, ref);
        check_bv_file(dir, a, ref);
        std::vector<std::uint16_t> on_disk;
        REQUIRE(cache::load_dip(dir, a.B, tag, on_disk));
        CHECK(on_disk == dip); // the streamed dip file is in natural residue order
    }
    // A dip file with other values: b's bitvectors must come from it, so the
    // build loaded the file instead of walking the dips.
    std::vector<std::uint16_t> planted = dip;
    for (std::size_t r = 1; r < planted.size(); r += 7) {
        planted[r] = 0;
    }
    REQUIRE(cache::store_dip(dir, b.B, tag, planted));
    const Bitvectors ref = reference(b, planted);
    REQUIRE(ref != reference(b, dip));
    {
        const Filter f(params(b, true)); // bitvector miss, dip hit
        CHECK_FALSE(f.bv_cache_hit());
        check_filter(f, b, ref);
        check_bv_file(dir, b, ref);
    }
    collatz::test::unset_env("COLLATZ_CACHE_DIR");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace cold_build
