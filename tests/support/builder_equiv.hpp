#pragma once

// Shared helpers for the Step-2 dip-builder tests:
//   * the residue-locality premise of the window gain tables, checked against
//     an independent brute-force backward-word search on real integers, and
//   * byte comparison of the dip cache file and the packed-bitvector cache file
//     written by two fresh builds of one config at different thread counts (the
//     per-residue builder below B = 14, the class-major one from B = 14).
// Used by tests/core/test_step2_builders.cpp.

#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "test_env.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <process.h>
#include <string>
#include <system_error>
#include <vector>

namespace collatz::test::builder_equiv {

using collatz::u128;

// Independent brute force: best 485 v - 306 L over backward T-words from the
// real integer x (no residue, no lift) with L <= depth total steps, v odd steps,
// 1 <= v <= min(o, t), and 2^L < 3^v. kNoGain when none. Written from the
// definition, independently of WindowChainTable::explore.
inline std::int32_t brute_gain(u128 x, unsigned o, unsigned t, unsigned depth) {
    const unsigned cap = std::min(o, t);
    std::int32_t best = preimage::WindowChainTable::kNoGain;
    struct Node {
        u128 y;
        unsigned L, v;
    };
    std::vector<Node> stack{{x, 0, 0}};
    while (!stack.empty()) {
        const Node n = stack.back();
        stack.pop_back();
        if (n.v >= 1 && (u128{1} << n.L) < pow3(n.v)) {
            best = std::max(
                best, 485 * static_cast<std::int32_t>(n.v) - 306 * static_cast<std::int32_t>(n.L));
        }
        if (n.L == depth) {
            continue;
        }
        stack.push_back({2 * n.y, n.L + 1, n.v}); // u = 2y: T(u) = y.
        if (n.v < cap && n.y % 3 == 2) {
            stack.push_back({(2 * n.y - 1) / 3, n.L + 1, n.v + 1}); // T(u) = (3u+1)/2 = y.
        }
    }
    return best;
}

// The premise for one table: residue_local() plus an explicit exhaustive
// gain(c, o) == gain(c mod 3^o, o) over the whole domain, plus brute_gain on
// x = c + k 3^o (k = 0, 1, 7, 3^t-scale) for classes c on a stride (`stride`=1
// is exhaustive). Returns the number of brute-force comparisons made.
inline std::uint64_t check_premise(unsigned t, unsigned depth, std::uint64_t stride) {
    const preimage::WindowChainTable tab(t, depth);
    REQUIRE(tab.residue_local());
    const auto mod = static_cast<std::uint64_t>(tab.modulus());
    for (unsigned o = 0; o <= t; ++o) {
        const auto m = static_cast<std::uint64_t>(pow3(o));
        for (std::uint64_t c = 0; c < mod; ++c) {
            if (tab.gain(c, o) != tab.gain(c % m, o)) {
                FAIL("gain not residue-local: t=" << t << " depth=" << depth << " c=" << c
                                                  << " o=" << o);
            }
        }
    }
    std::uint64_t n = 0;
    for (std::uint64_t c = 0; c < mod; c += stride) {
        for (unsigned o = 1; o <= t; ++o) {
            const u128 m = pow3(o);
            for (const u128 k : {u128{0}, u128{1}, u128{7}, u128{mod} * 5u + 3u}) {
                const u128 x = static_cast<u128>(c) + k * m;
                const std::int32_t want = tab.gain(static_cast<u128>(c), o);
                const std::int32_t got = brute_gain(x, o, t, depth);
                if (got != want) {
                    FAIL("brute mismatch t=" << t << " depth=" << depth << " c=" << c << " o=" << o
                                             << " got=" << got << " table=" << want);
                }
                ++n;
            }
        }
    }
    return n;
}

inline std::string temp_dir(const std::string& tag) {
    const auto pid = _getpid();
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("collatz_beq_" + tag + "_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}

// FNV-1a over a file's bytes (streamed), tagged with its size.
inline std::uint64_t file_hash(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::uint64_t h = 1469598103934665603ULL;
    std::vector<char> buf(std::size_t{1} << 20);
    std::uint64_t size = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        for (std::size_t i = 0; i < got; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
        size += got;
    }
    return h ^ (size * 0x9E3779B97F4A7C15ULL);
}

struct Cfg {
    unsigned B, tw, D;
};

// Fresh cached build of one config on `threads` threads into a private temp dir;
// returns {cache filename -> content hash} and deletes the dir. The dip file is
// written by the dip build (natural order), the bv file by the Filter.
inline std::map<std::string, std::uint64_t> build_files(const Cfg& c, unsigned threads) {
    const std::string dir = temp_dir("b" + std::to_string(c.B) + "_" + std::to_string(threads));
    set_env("COLLATZ_CACHE_DIR", dir.c_str());
    step2::Params p;
    p.B = c.B;
    p.A = 10;
    p.N = c.B + 16u; // M = B + 6 >= B.
    p.num_bitvectors = 8;
    p.window_chains = c.tw;
    p.window_templates_D = c.D;
    p.threads = threads;
    p.use_bv_cache = true;
    {
        const step2::Filter f(p);
        REQUIRE(!f.bitvectors().empty());
        REQUIRE_FALSE(f.bv_cache_hit());
    }
    unset_env("COLLATZ_CACHE_DIR");
    std::map<std::string, std::uint64_t> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        out[e.path().filename().string()] = file_hash(e.path());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return out;
}

// Two fresh builds write byte-identical dip and bv cache files. The class-major
// builder runs on max(threads, hardware_threads()) threads, so the second build
// uses more than the hardware has.
inline void check_builders(const Cfg& c) {
    CAPTURE(c.B);
    CAPTURE(c.tw);
    CAPTURE(c.D);
    const auto first = build_files(c, hardware_threads());
    REQUIRE(first.size() == 2u); // dip + bv
    const std::string tag = "_tw" + std::to_string(c.tw + 32u * c.D) + "_v4";
    REQUIRE(first.count("dip_B" + std::to_string(c.B) + tag + ".u16") == 1u);
    CHECK(build_files(c, 2u * hardware_threads() + 1u) == first);
}

} // namespace collatz::test::builder_equiv
