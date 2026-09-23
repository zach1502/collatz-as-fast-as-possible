// The builder fingerprint of the Step-2 cache files: a hash of what this
// binary's table builder produces on small fixed inputs. A file records the
// fingerprint of the binary that wrote it, and a reader uses the file only
// when its own fingerprint is equal, so a builder change made without a format
// bump, a dev build with a different builder, or a toolchain that compiles the
// builder differently cannot hand its tables to another binary.
//
// It is computed at run time rather than from the builder's source files at
// build time: it covers whatever code runs (the transitive headers, the
// compiler and its flags included), needs no build-system step that can go
// stale between configure and build, and does not change on edits that only
// touch comments or line endings (which would rebuild a 12 GiB B=32 cache). Its
// limit is coverage: a change that alters no table at these inputs keeps the
// fingerprint. It takes a few milliseconds once per process.
//
// The dip values themselves are hashed, not only the bitvectors packed from
// them: every margin and every threshold has the form 306 x - 485 y, so a small
// change of a dip value flips no bit at the thresholds of one N - A, while the
// dip file serves every N - A.

#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "collatz/threading.hpp"
#include "step2_cache_internal.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace collatz::step2::cache {

namespace {

std::uint64_t compute_fingerprint() {
    detail::Xxh64 h;
    const auto feed = [&h](const auto& v) { h.update(v.data(), v.size() * sizeof(v[0])); };
    const std::array<std::uint32_t, 1> format{detail::kFormatVersion};
    feed(format);
    const unsigned threads = std::max(1u, hardware_threads());
    // The shipped window-chain table (t_w = 6) and a merge-template table made
    // by the same code at depth 8 (the shipped depth 12 takes about 0.3 s).
    const preimage::WindowChainTable wct(6u);
    const preimage::WindowChainTable wtt(8u, 8u);
    constexpr unsigned kM = 30; // N - A of `verify --N 40`.
    constexpr unsigned kFmin = (306u * kM + 485u) / 485u;
    // B = 18 takes the class-major build with full 16-word tiles, and B = 12
    // the natural-order dip table and packer (4096 residues, so one thread).
    for (const unsigned B : {18u, 12u}) {
        const unsigned nt = B >= 14u ? threads : 1u;
        const step2::detail::BuiltLevel lv =
            step2::detail::build_level(B, kFmin, kM, 8u, nt, std::string(), &wct, 0u, &wtt);
        for (const std::vector<std::uint64_t>& bv : lv.bvs) {
            feed(bv);
        }
        feed(lv.popcount);
        feed(lv.inv3f);
    }
    // Every dip of the class-major walk at B = 16 without window terms (the
    // walk build_level runs when both tables are off).
    constexpr unsigned kCmB = 16;
    const std::unique_ptr<std::uint16_t[]> cm(new std::uint16_t[std::size_t{1} << kCmB]);
    step2::detail::walk_dip_classmajor(cm, step2::detail::DipGain{}, kCmB, 8u, threads);
    h.update(cm.get(), (std::size_t{1} << kCmB) * sizeof(std::uint16_t));
    // Every dip of the per-residue walk at B = 14 with both tables, with the
    // window-chain table alone (a template table hides most chain terms), and
    // with neither; then walks of the full max_B steps, which the small tables
    // above never take.
    const preimage::WindowChainTable* const pairs[3][2] = {
        {&wct, &wtt}, {&wct, nullptr}, {nullptr, nullptr}};
    std::vector<std::uint32_t> dips(std::size_t{3} << 14);
    step2::detail::run_chunked(threads, dips.size(), 1024, [&](std::uint64_t lo, std::uint64_t hi) {
        for (std::uint64_t j = lo; j < hi; ++j) {
            const auto& t = pairs[j >> 14];
            dips[static_cast<std::size_t>(j)] = Filter::dip(j & 0x3FFFu, 14u, t[0], t[1]);
        }
    });
    feed(dips);
    std::vector<std::uint32_t> deep(4096);
    step2::detail::run_chunked(threads, deep.size(), 256, [&](std::uint64_t lo, std::uint64_t hi) {
        for (std::uint64_t k = lo; k < hi; ++k) {
            deep[static_cast<std::size_t>(k)] =
                Filter::dip((k + 1u) * 0x9E3779B97F4A7C15ull, max_B, &wct, &wtt);
        }
    });
    feed(deep);
    return h.digest();
}

} // namespace

std::uint64_t builder_fingerprint() {
    static const std::uint64_t fp = compute_fingerprint();
    return fp;
}

} // namespace collatz::step2::cache
