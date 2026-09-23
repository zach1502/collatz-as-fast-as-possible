// Step 2 class-major cold build: build_level_classmajor (the combined window gain
// table, the dip cache load or walk, the overlapped dip cache write and the tiled
// pack).

#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace collatz::step2 {

using namespace detail;

namespace {

/// Builds DipGain. Returns false if the tables cannot be combined in u16; the
/// caller then takes the natural-order path. Moduli are powers of 3, so the
/// smaller one divides the larger and gain_small(c mod m_small) is a function
/// of c.
bool make_dip_gain(DipGain& g,
                   const preimage::WindowChainTable* wct,
                   const preimage::WindowChainTable* wtt) {
    g = DipGain{};
    if (wct == nullptr && wtt == nullptr) {
        return true;
    }
    const unsigned t1 = wct != nullptr ? wct->t() : 0u;
    const unsigned t2 = wtt != nullptr ? wtt->t() : 0u;
    g.tc = std::max(t1, t2);
    const u128 m = pow3(g.tc);
    if (m > u128{1} << 24) {
        return false;
    }
    g.on = true;
    g.mod = static_cast<std::uint32_t>(m);
    g.stride = g.tc + 1u;
    const std::uint64_t m1 = wct != nullptr ? static_cast<std::uint64_t>(wct->modulus()) : 1u;
    const std::uint64_t m2 = wtt != nullptr ? static_cast<std::uint64_t>(wtt->modulus()) : 1u;
    g.id.resize(g.mod);
    std::vector<std::uint16_t> row(g.stride);
    // Rows are deduplicated through an open-addressing hash of their bytes.
    std::vector<std::int32_t> slot(std::size_t{1} << 16, -1);
    const auto hash_row = [&row]() {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::uint16_t x : row) {
            h = (h ^ x) * 1099511628211ULL;
        }
        return h;
    };
    unsigned nrows = 0;
    for (std::uint32_t c = 0; c < g.mod; ++c) {
        row[0] = 0; // o = 0 never occurs (the term needs f >= 1).
        for (unsigned o = 1; o <= g.tc; ++o) {
            std::int32_t best = 0; // only positive gains ever raise the max.
            if (wct != nullptr) {
                best = std::max(best, wct->gain(static_cast<u128>(c % m1), std::min(o, t1)));
            }
            if (wtt != nullptr) {
                best = std::max(best, wtt->gain(static_cast<u128>(c % m2), std::min(o, t2)));
            }
            if (best > 0xFFFF) {
                return false;
            }
            row[o] = static_cast<std::uint16_t>(best);
        }
        std::size_t s = static_cast<std::size_t>(hash_row()) & (slot.size() - 1u);
        for (;;) {
            if (slot[s] < 0) {
                // The probe table stays at most half full, and every row offset
                // (the value stored in id[]) fits a uint16.
                if (nrows >= 0x8000u || (nrows + 1u) * g.stride > 0x10000u) {
                    return false;
                }
                slot[s] = static_cast<std::int32_t>(nrows);
                g.rows.insert(g.rows.end(), row.begin(), row.end());
                g.id[c] = static_cast<std::uint16_t>(nrows * g.stride);
                ++nrows;
                break;
            }
            const std::uint16_t* cand = &g.rows[static_cast<std::size_t>(slot[s]) * g.stride];
            if (std::equal(row.begin(), row.end(), cand)) {
                g.id[c] = static_cast<std::uint16_t>(static_cast<unsigned>(slot[s]) * g.stride);
                break;
            }
            s = (s + 1u) & (slot.size() - 1u);
        }
    }
    return true;
}

// The cold build runs before any verifier worker exists, so it uses
// max(--threads, hardware_threads()) threads, even under --threads 1. The output
// does not depend on the count.
[[nodiscard]] unsigned cold_build_threads(unsigned threads) {
    const unsigned hw = std::max(1u, hardware_threads());
    return std::max(threads, hw);
}

} // namespace

namespace detail {

/// The class-major build. Fills `out` and returns true, or returns false
/// (nothing built) when the layout or gain table does not apply; the caller then
/// runs the natural-order path.
bool build_level_classmajor(BuiltLevel& out,
                            unsigned B,
                            unsigned nbv,
                            unsigned threads,
                            const std::vector<std::int64_t>& thr,
                            unsigned fmin,
                            const std::string& cache_dir,
                            const preimage::WindowChainTable* wct,
                            unsigned tw,
                            const preimage::WindowChainTable* wtt) {
    // Row width H = 8. The layout needs H + 6 <= B; smaller B takes the
    // natural-order path.
    constexpr unsigned H = 8;
    if (B < 14 || nbv == 0) {
        return false;
    }
    DipGain g;
    if (!make_dip_gain(g, wct, wtt)) {
        return false;
    }
    const unsigned nthreads = cold_build_threads(threads);
    const std::uint64_t span = std::uint64_t{1} << B;

    // Uninitialized (every entry is written exactly once below), so the 2 GB
    // first touch happens in parallel in the workers, not in a serial fill.
    std::unique_ptr<std::uint16_t[]> cm(new std::uint16_t[span]);
    bool dip_hit = false;
    if (!cache_dir.empty()) {
        dip_hit = cache::load_dip_classmajor(cache_dir, B, tw, H, nthreads, cm.get());
    }
    if (!dip_hit) {
        walk_dip_classmajor(cm, g, B, H, nthreads);
    }

    // The dip-cache write (natural order, byte-identical to store_dip) overlaps
    // the pack; both only read cm. The cache is best effort and must not fail a
    // verification: the body swallows every exception (store_dip_classmajor
    // returns false and drops its temp file on failure), and if the writer
    // thread cannot be spawned the store is skipped (the next cold run rebuilds).
    std::thread writer;
    // COLLATZ_DIP_CACHE=0 skips writing the dip cache (2^B x 2 bytes: 8 GiB at
    // B=32). The bitvector cache that warm runs load is written regardless, so a
    // machine short on disk can still cache B=32 (4 GiB) and skip the 8 GiB file.
    const bool dip_store_on = [] {
        const char* e = std::getenv("COLLATZ_DIP_CACHE");
        return e == nullptr || e[0] == '\0' || e[0] != '0';
    }();
    if (!dip_hit && !cache_dir.empty() && dip_store_on) {
        const auto store = [&]() noexcept {
            try {
                (void) cache::store_dip_classmajor(cache_dir, B, tw, H, nthreads, cm.get());
            } catch (...) { // NOLINT(bugprone-empty-catch): best effort, no dip cache file
            }
        };
        try {
            writer = std::thread(store);
        } catch (...) { // NOLINT(bugprone-empty-catch): the dip cache is best effort
            // On std::system_error there is no writer thread and no store.
        }
    }

    std::vector<std::atomic<std::uint64_t>> popc(nbv);
    alloc_level(out, popc, nbv, fmin, B);
    pack_tiles(out, popc, cm, thr, B, H, nbv, fmin, nthreads);
    for (unsigned i = 0; i < nbv; ++i) {
        out.popcount[i] = popc[i].load();
    }
    if (writer.joinable()) {
        writer.join();
    }
    return true;
}

} // namespace detail

} // namespace collatz::step2
