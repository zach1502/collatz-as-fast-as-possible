#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "step2_cache_dir.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// Unit coverage for the on-disk Step-2 bitvector/dip cache
// (collatz/step2_cache.hpp). Everything here uses a per-process temp cache dir
// and small B, so it never touches a user's real %LOCALAPPDATA%\collatz and never
// depends on any cache file pre-existing. The cache is a pure speedup, so these
// checks pin that a load reproduces a store byte-for-byte and that any
// corruption/key mismatch is rejected (forcing a rebuild), never misread.

namespace cache = collatz::step2::cache;
using collatz::step2::Filter;
using collatz::test::unique_tmp_dir;

namespace {

std::vector<std::uint16_t> dip_table(unsigned B) {
    std::vector<std::uint16_t> dip(std::size_t{1} << B);
    for (std::uint64_t m = 0; m < dip.size(); ++m) {
        dip[m] = static_cast<std::uint16_t>(Filter::dip(m, B, nullptr, nullptr));
    }
    return dip;
}

// Header layout of format version 5: u64 magic, u32 version at 8, ..., u64
// payload_bytes at 40, u64 builder at 48, u64 payload_hash at 56.
template <class T>
void patch_header(const std::string& path, std::streamoff at, T v) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(at);
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

// Bitvectors for `key` with arbitrary words and the inv3f a Filter computes.
struct BvFixture {
    std::vector<std::vector<std::uint64_t>> bvs;
    std::vector<std::uint64_t> inv3f;
    std::vector<std::uint64_t> popc;
};

BvFixture bv_fixture(const cache::BvKey& key) {
    const std::uint64_t words = ((std::uint64_t{1} << key.B) + 63u) / 64u;
    BvFixture fx;
    fx.bvs.assign(key.nbv, std::vector<std::uint64_t>(words, 0));
    for (unsigned i = 0; i < key.nbv; ++i) {
        fx.inv3f.push_back(Filter::inverse_pow3_mod2b(key.fmin + i, key.B));
        std::uint64_t p = 0;
        for (std::uint64_t w = 0; w < words; ++w) {
            fx.bvs[i][w] = (0x1234567ull * (w + 1u)) ^ (0xabcdefull * (i + 3u));
            p += static_cast<std::uint64_t>(std::popcount(fx.bvs[i][w]));
        }
        fx.popc.push_back(p);
    }
    return fx;
}

} // namespace

TEST_CASE("dip cache round-trips and rejects corruption") {
    const std::string dir = unique_tmp_dir();
    REQUIRE_FALSE(dir.empty());

    constexpr unsigned B = 16;
    const std::vector<std::uint16_t> dip = dip_table(B);

    CHECK(cache::store_dip(dir, B, 0u, dip));

    std::vector<std::uint16_t> loaded;
    REQUIRE(cache::load_dip(dir, B, 0u, loaded));
    REQUIRE(loaded.size() == dip.size());
    CHECK(loaded == dip);

    // A different B is a different key -> miss (no file), not a misread.
    std::vector<std::uint16_t> other;
    CHECK_FALSE(cache::load_dip(dir, B + 1u, 0u, other));

    // A different window-chain tag tw is a different key -> miss.
    std::vector<std::uint16_t> other_tw;
    CHECK_FALSE(cache::load_dip(dir, B, 3u, other_tw));

    // One extra byte, then a truncation: the size check rejects both, so the
    // caller rebuilds. std::filesystem::resize_file is the portable truncate.
    const std::string path = dir + "/dip_B" + std::to_string(B) + "_tw0_v4.u16";
    const std::uintmax_t size = std::filesystem::file_size(path);
    std::error_code ec;
    std::filesystem::resize_file(path, size + 1u, ec);
    REQUIRE_FALSE(ec);
    std::vector<std::uint16_t> after;
    CHECK_FALSE(cache::load_dip(dir, B, 0u, after));
    std::filesystem::resize_file(path, 128, ec);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(cache::load_dip(dir, B, 0u, after));
}

TEST_CASE("files of another format version or another builder are never loaded") {
    // The header carries format version 5 and the writer's builder
    // fingerprint. A file of the same key with version 4 (the format before
    // the payload hash) or with another fingerprint is not trusted; restoring
    // the field makes the file load again, so the field alone decided.
    const std::string dir = unique_tmp_dir();
    REQUIRE_FALSE(dir.empty());
    constexpr unsigned B = 12;
    REQUIRE(cache::store_dip(dir, B, 0u, dip_table(B)));
    const std::string dip = dir + "/dip_B12_tw0_v4.u16";
    REQUIRE(std::filesystem::exists(dip));
    std::vector<std::uint16_t> got;
    REQUIRE(cache::load_dip(dir, B, 0u, got));
    patch_header<std::uint32_t>(dip, 8, 4u);
    CHECK_FALSE(cache::load_dip(dir, B, 0u, got));
    patch_header<std::uint32_t>(dip, 8, 5u);
    CHECK(cache::load_dip(dir, B, 0u, got));
    const std::uint64_t fp = cache::builder_fingerprint();
    patch_header<std::uint64_t>(dip, 48, fp ^ 1u);
    CHECK_FALSE(cache::load_dip(dir, B, 0u, got));
    patch_header<std::uint64_t>(dip, 48, fp);
    CHECK(cache::load_dip(dir, B, 0u, got));

    // Bitvector file: same treatment, plus a payload_bytes that disagrees
    // with the key.
    const cache::BvKey key{12u, 20u, 2u, 7u};
    const BvFixture fx = bv_fixture(key);
    REQUIRE(cache::store_bitvectors(dir, key, fx.bvs, fx.inv3f, fx.popc));
    const std::string bv = dir + "/bv_B12_M20_k2_tw0_v4.bin";
    REQUIRE(std::filesystem::exists(bv));
    REQUIRE(cache::map_bitvectors(dir, key).ok);
    patch_header<std::uint32_t>(bv, 8, 4u);
    CHECK_FALSE(cache::map_bitvectors(dir, key).ok);
    patch_header<std::uint32_t>(bv, 8, 5u);
    REQUIRE(cache::map_bitvectors(dir, key).ok);
    patch_header<std::uint64_t>(bv, 48, fp + 1u);
    CHECK_FALSE(cache::map_bitvectors(dir, key).ok);
    patch_header<std::uint64_t>(bv, 48, fp);
    REQUIRE(cache::map_bitvectors(dir, key).ok);
    patch_header<std::uint64_t>(bv, 40, std::uint64_t{0});
    CHECK_FALSE(cache::map_bitvectors(dir, key).ok);
}

TEST_CASE("the builder fingerprint is fixed for the process") {
    const std::uint64_t fp = cache::builder_fingerprint();
    CHECK(fp != 0u);
    CHECK(cache::builder_fingerprint() == fp);
}

TEST_CASE("bitvector cache round-trips and is key-checked") {
    const std::string dir = unique_tmp_dir();
    REQUIRE_FALSE(dir.empty());

    const cache::BvKey key{16u, 20u, 4u, 7u};
    const std::uint64_t words = (std::uint64_t{1} << key.B) / 64u;
    const BvFixture fx = bv_fixture(key);

    CHECK(cache::store_bitvectors(dir, key, fx.bvs, fx.inv3f, fx.popc));

    // The load maps the file in place: every word, inv3f and popcount as stored.
    const cache::MappedBitvectors m = cache::map_bitvectors(dir, key);
    REQUIRE(m.ok);
    REQUIRE(m.words == words);
    CHECK(m.inv3f == fx.inv3f);
    CHECK(m.popcount == fx.popc);
    REQUIRE(m.bv.size() == key.nbv);
    for (unsigned i = 0; i < key.nbv; ++i) {
        CHECK(std::equal(fx.bvs[i].begin(), fx.bvs[i].end(), m.bv[i]));
    }

    // Any key field mismatch is a miss (the file name encodes B/M/nbv; fmin is
    // validated from the header), so a stale/foreign file is never misread.
    cache::BvKey wrong = key;
    wrong.fmin = key.fmin + 1u;
    CHECK_FALSE(cache::map_bitvectors(dir, wrong).ok);
}
