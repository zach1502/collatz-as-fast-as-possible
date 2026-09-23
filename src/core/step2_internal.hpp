#pragma once

// Private declarations shared by the Step 2 sources: the rational-bound weights,
// the built lookahead level, the class-major layout and its combined gain table,
// the chunked thread runner, the worker-count clamp, the partial-OEE prefix mask,
// and the builder functions that one source calls in another.

#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace collatz::step2::detail {

/// Rational bound constants: 306*log2(3) is just below 485, so A_k = 306 k - 485 f_k
/// measures the contraction margin (2^k vs 3^{f_k}).
constexpr std::int64_t kHalveWeight = 306; ///< weight of one even step.
constexpr std::int64_t kOddWeight = 485;   ///< weight of one odd step.

/// One built lookahead level: `num_bitvectors` packed 2^B-bit vectors, their
/// set-bit counts, and inv(3^{fmin+i}) mod 2^B.
struct BuiltLevel {
    std::vector<std::vector<std::uint64_t>> bvs;
    std::vector<std::uint64_t> popcount;
    std::vector<std::uint64_t> inv3f;
};

// Class-major cold build: a class-major dip table and a tiled pack. Where it
// does not apply (B < 14, a window table that is not residue-local, or gains
// that do not fit the u16 DipGain) the natural-order path runs (natural-order
// table, per-residue walk, one permuted pass per bitvector). Both give the same
// bytes.
//
// The natural-order packer reads dip[j * 3^f mod 2^B] for j = 0, 1, 2, ...: one
// random access into a 2 GB table per bit (TLB and DRAM miss), nbv times over.
// Write j = jl + 2^L jh and r = j * s mod 2^B (s = 3^f odd, L = B - H). Then
//   r mod 2^L = jl * s mod 2^L                        (fixed by jl), and
//   r >> L    = ((jl * s mod 2^B) >> L) + jh * s  mod 2^H.
// With the table stored class-major, cm[((r mod 2^L) << H) | (r >> L)], every jl
// reads one contiguous 2^H-entry row (512 B at H = 8) with stride s, and bit jl
// of output word (jl >> 6) + jh * 2^(L-6) comes from row entry jh. A tile of
// 64 G consecutive jl reads 64 G rows (L1/L2 resident) and writes G adjacent
// words per jh (one cache line at G = 16), so the pack is a streaming pass. The
// walk fills the same layout: a row is one prefix class c = r mod 2^L, walked
// L steps once and then B - L = H steps per residue c + 2^L h.
//
// The walk does one window-gain lookup per step, in a combined, row-deduplicated
// table (DipGain), instead of two (a 27 MB merge-template table indexed by
// v % 3^12 and the window-chain table indexed by v % 3^6, each a runtime
// division and a cache miss). At t_w = 6, D = 12 DipGain has 275 distinct rows: a
// 1 MB u16 row-id array and a 7 KB row table, indexed by v mod 3^t, which the
// walk keeps without division. Both are max-folds of the same terms, so dip is
// the same.

/// Combined window-chain and merge-template gain, max(g_wct, g_wtt, 0),
/// deduplicated by row. `rows[id[c] + o]` (o = min(f, tc)) is the gain of class
/// c mod `mod`.
struct DipGain {
    bool on = false;               ///< false: no window terms (both tables null).
    std::uint32_t mod = 1;         ///< 3^tc (the larger table's modulus).
    unsigned tc = 0;               ///< max(t_wct, t_wtt).
    unsigned stride = 1;           ///< tc + 1 (row length).
    std::vector<std::uint16_t> id; ///< row offset (row index * stride) per class.
    std::vector<std::uint16_t> rows;
};

// Runs body over [0, units) in dynamically claimed chunks of `grain`, which
// balances the load on a busy machine. Each unit's result depends only on the
// unit, so the schedule does not matter.
template <class Body>
void run_chunked(unsigned nthreads, std::uint64_t units, std::uint64_t grain, const Body& body) {
    std::atomic<std::uint64_t> next{0};
    const auto worker = [&]() {
        for (;;) {
            const std::uint64_t lo = next.fetch_add(grain);
            if (lo >= units) {
                return;
            }
            body(lo, std::min(lo + grain, units));
        }
    };
    std::vector<std::thread> pool;
    const unsigned nt =
        static_cast<unsigned>(std::min<std::uint64_t>(nthreads, (units + grain - 1u) / grain));
    try {
        pool.reserve(nt);
        for (unsigned t = 1; t < nt; ++t) {
            pool.emplace_back(worker);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): fewer threads finish the range
        // A failed spawn (std::system_error, bad_alloc) is harmless: units are
        // claimed dynamically, so the running threads and this one finish the
        // range.
    }
    worker();
    for (std::thread& th : pool) {
        th.join();
    }
}

// span reaches 2^B (2^32 at B=32), so the worker count is clamped in 64-bit:
// a 32-bit cast truncates 2^32 to 0, spawning no workers and leaving the
// table zero.
inline unsigned clamp_threads(unsigned threads, std::uint64_t units) {
    unsigned n = threads == 0 ? 1u : threads;
    return static_cast<unsigned>(std::min<std::uint64_t>(n, units == 0 ? 1u : units));
}

/// A CandidateMask with the low `nbits` (0 <= nbits <= 2^A) bits set, across
/// `words` words. Used for the partial-OEE "keep a < a*" prefix. Writes all
/// `words` words (the tail above `nbits` is zeroed explicitly), so it is safe on
/// the not-auto-zeroed CandidateMask.
[[nodiscard]] static inline CandidateMask prefix_mask(unsigned nbits, unsigned words) noexcept {
    CandidateMask m;
    m.words = words;
    const unsigned full = nbits / 64u;
    for (unsigned wi = 0; wi < full && wi < words; ++wi) {
        m.w[wi] = ~std::uint64_t{0};
    }
    unsigned next = full;
    const unsigned rem = nbits & 63u;
    if (rem != 0u && full < words) {
        m.w[full] = (std::uint64_t{1} << rem) - 1;
        next = full + 1u;
    }
    for (unsigned wi = next; wi < words; ++wi) {
        m.w[wi] = 0; // zero the tail (mask is not auto-zeroed).
    }
    return m;
}

// Defined in step2_coldbv_walk.cpp.
void walk_dip_classmajor(const std::unique_ptr<std::uint16_t[]>& cm,
                         const DipGain& g,
                         unsigned B,
                         unsigned H,
                         unsigned nthreads);

// Defined in step2_pack.cpp.
void alloc_level(BuiltLevel& out,
                 std::vector<std::atomic<std::uint64_t>>& popc,
                 unsigned nbv,
                 unsigned fmin,
                 unsigned B);
void pack_tiles(BuiltLevel& out,
                std::vector<std::atomic<std::uint64_t>>& popc,
                const std::unique_ptr<std::uint16_t[]>& cm,
                const std::vector<std::int64_t>& thr,
                unsigned B,
                unsigned H,
                unsigned nbv,
                unsigned fmin,
                unsigned nthreads);
void pack_level(BuiltLevel& out,
                const std::vector<std::int64_t>& thr,
                const std::vector<std::uint16_t>& dip_table,
                unsigned B,
                unsigned fmin,
                unsigned nbv,
                unsigned threads);

// Defined in step2_coldbv.cpp.
bool build_level_classmajor(BuiltLevel& out,
                            unsigned B,
                            unsigned nbv,
                            unsigned threads,
                            const std::vector<std::int64_t>& thr,
                            unsigned fmin,
                            const std::string& cache_dir,
                            const preimage::WindowChainTable* wct,
                            unsigned tw,
                            const preimage::WindowChainTable* wtt);

// Defined in step2_build.cpp.
BuiltLevel build_level(unsigned B,
                       unsigned fmin,
                       unsigned m_bits,
                       unsigned nbv,
                       unsigned threads,
                       const std::string& cache_dir,
                       const preimage::WindowChainTable* wct,
                       unsigned tw,
                       const preimage::WindowChainTable* wtt);

} // namespace collatz::step2::detail
