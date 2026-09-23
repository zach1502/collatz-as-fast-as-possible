// Step-2 cache integrity (src/core/step2_cache*.cpp): a cache file with one
// zeroed word, or with an inv3f that does not belong to its key, is never used.
// The Filter and verify::run rebuild the table and get exactly the result of a
// clean cache. Each case has a cache dir of its own (B = 20, a few ms to build).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "collatz/verify.hpp"
#include "step2_cache_dir.hpp"

#include <bit>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace cache = collatz::step2::cache;
namespace vf = collatz::verify;
using collatz::step2::Filter;
using collatz::step2::Params;
using collatz::test::fresh_cache_dir;
using collatz::test::patch_payload;
using collatz::test::payload_word;
using Tables = std::vector<std::vector<std::uint64_t>>;

constexpr unsigned kN = 32;
constexpr unsigned kA = 10;
constexpr unsigned kB = 20;
constexpr unsigned kNbv = 8;
constexpr unsigned kTw = 6; // the shipped window-chain cap, D = 0.
constexpr unsigned kM = kN - kA;
constexpr unsigned kFmin = (306u * kM + 485u) / 485u;
constexpr std::uint64_t kWords = (std::uint64_t{1} << kB) / 64u;
constexpr std::uint64_t kBvPayload = (2u * kNbv + kNbv * kWords) * 8u;
constexpr std::uint64_t kDipPayload = (std::uint64_t{1} << kB) * 2u;
const std::vector<unsigned char> kZeroWord(8, 0);

Params params(bool use_cache) {
    Params p;
    p.N = kN;
    p.A = kA;
    p.B = kB;
    p.num_bitvectors = kNbv;
    p.threads = 4;
    p.window_chains = kTw;
    p.use_bv_cache = use_cache;
    return p;
}

vf::Options options() {
    vf::Options o;
    o.N = kN;
    o.A = kA;
    o.B = kB;
    o.num_bitvectors = kNbv;
    o.threads = 4;
    o.window_chains = kTw;
    o.bv_cache = true;
    return o;
}

std::string bv_file(const std::string& dir) {
    return dir + "/bv_B20_M22_k8_tw6_v4.bin";
}

std::string dip_file(const std::string& dir) {
    return dir + "/dip_B20_tw6_v4.u16";
}

Tables tables(const Filter& f) {
    Tables t;
    for (const auto& span : f.bitvectors()) {
        t.emplace_back(span.begin(), span.end());
    }
    return t;
}

// Zeroes the first nonzero word at payload offset from, from + step, ... and
// returns its offset (0 when there is none). The start in the middle of the
// file and a step of 512 bytes keep the word away from the bytes that a sampled
// check reads (the head, the tail, and the first 64 of every 256 or 512 bytes).
std::uint64_t zero_word(const std::string& path, std::uint64_t payload, std::uint64_t from) {
    for (std::uint64_t off = from; off + 8u <= payload; off += 512u) {
        if (payload_word(path, payload, off) != 0u) {
            REQUIRE(patch_payload(path, payload, off, kZeroWord).size() == 8u);
            return off;
        }
    }
    return 0;
}

// The middle of bitvector 0, which most survivors read, at an offset of 128
// mod 256; and a dip entry at 256 mod 512.
constexpr std::uint64_t kBvWordFrom = 2u * kNbv * 8u + kWords * 4u;
constexpr std::uint64_t kDipWordFrom = kDipPayload / 2u + 256u;

} // namespace

TEST_CASE("a zeroed bitvector word is rejected and the table rebuilt") {
    const std::string dir = fresh_cache_dir("bv_word");
    const Tables clean = tables(Filter(params(false)));
    {
        const Filter miss(params(true));
        CHECK_FALSE(miss.bv_cache_hit());
    }
    REQUIRE(zero_word(bv_file(dir), kBvPayload, kBvWordFrom) != 0u);
    const Filter after(params(true));
    CHECK_FALSE(after.bv_cache_hit());
    CHECK(tables(after) == clean);
    // The rebuilt file replaced the damaged one.
    const Filter again(params(true));
    CHECK(again.bv_cache_hit());
    CHECK(tables(again) == clean);
}

TEST_CASE("a zeroed dip word is rejected and the dips walked again") {
    const std::string dir = fresh_cache_dir("dip_word");
    const Tables clean = tables(Filter(params(false)));
    {
        const Filter miss(params(true));
    }
    // Without the bitvector file the Filter packs from the dip file.
    std::filesystem::remove(bv_file(dir));
    REQUIRE(zero_word(dip_file(dir), kDipPayload, kDipWordFrom) != 0u);
    const Filter after(params(true));
    CHECK_FALSE(after.bv_cache_hit());
    CHECK(tables(after) == clean);
}

TEST_CASE("a bitvector file whose inv3f does not belong to its key is rejected") {
    const std::string dir = fresh_cache_dir("inv3f");
    const Filter clean(params(false));
    std::vector<std::uint64_t> inv3f;
    std::vector<std::uint64_t> popc;
    for (unsigned i = 0; i < kNbv; ++i) {
        inv3f.push_back(clean.inv3f(i));
        std::uint64_t p = 0;
        for (const std::uint64_t w : clean.bitvectors()[i]) {
            p += static_cast<std::uint64_t>(std::popcount(w));
        }
        popc.push_back(p);
    }
    const cache::BvKey key{kB, kM, kNbv, kFmin, kTw};
    // A consistent file (its checksum is the writer's own) with inv3f[3] = 0.
    std::vector<std::uint64_t> wrong = inv3f;
    wrong[3] = 0;
    REQUIRE(cache::store_bitvectors(dir, key, tables(clean), wrong, popc));
    CHECK_FALSE(cache::map_bitvectors(dir, key).ok);
    const Filter after(params(true));
    CHECK_FALSE(after.bv_cache_hit());
    for (unsigned i = 0; i < kNbv; ++i) {
        CHECK(after.inv3f(i) == inv3f[i]);
    }
    // A zeroed inv3f word in a good file is rejected too.
    REQUIRE(patch_payload(bv_file(dir), kBvPayload, 8u * 5u, kZeroWord).size() == 8u);
    CHECK_FALSE(cache::map_bitvectors(dir, key).ok);
}

TEST_CASE("verify::run after one zeroed cache word: a rebuild and the clean result") {
    const std::string dir = fresh_cache_dir("run");
    const vf::Options o = options();
    const vf::Result clean = vf::run(o);
    REQUIRE_FALSE(clean.bv_cache_hit);
    const auto same = [&clean](const vf::Result& r) {
        CHECK(r.candidates_checked == clean.candidates_checked);
        CHECK(r.coverage_checksum == clean.coverage_checksum);
        CHECK(r.pow_checksum == clean.pow_checksum);
        CHECK(r.max_excursion == clean.max_excursion);
    };
    SUBCASE("a bitvector word") {
        REQUIRE(zero_word(bv_file(dir), kBvPayload, kBvWordFrom) != 0u);
        const vf::Result r = vf::run(o);
        CHECK_FALSE(r.bv_cache_hit);
        same(r);
    }
    SUBCASE("a dip word, with the bitvector file gone") {
        std::filesystem::remove(bv_file(dir));
        REQUIRE(zero_word(dip_file(dir), kDipPayload, kDipWordFrom) != 0u);
        const vf::Result r = vf::run(o);
        CHECK_FALSE(r.bv_cache_hit);
        same(r);
    }
    SUBCASE("inv3f, in a file written with a zeroed multiplier") {
        const Filter f(params(false));
        std::vector<std::uint64_t> inv3f(kNbv);
        std::vector<std::uint64_t> popc(kNbv, 0);
        for (unsigned i = 1; i < kNbv; ++i) {
            inv3f[i] = f.inv3f(i);
        }
        const cache::BvKey key{kB, kM, kNbv, kFmin, kTw};
        REQUIRE(cache::store_bitvectors(dir, key, tables(f), inv3f, popc));
        const vf::Result r = vf::run(o);
        CHECK_FALSE(r.bv_cache_hit);
        same(r);
    }
}
