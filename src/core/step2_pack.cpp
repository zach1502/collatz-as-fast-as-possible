// Step 2 bitvector packers: the class-major tiled pack with its zero-filled
// allocation, and the natural-order pack from the dip table.

#include "collatz/pow3.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

namespace collatz::step2::detail {

// Sizes the nbv zero-filled bitvectors and fills inv3f.
void alloc_level(BuiltLevel& out,
                 std::vector<std::atomic<std::uint64_t>>& popc,
                 unsigned nbv,
                 unsigned fmin,
                 unsigned B) {
    const std::uint64_t span = std::uint64_t{1} << B;
    const std::uint64_t words = span / 64u;
    out.bvs.assign(nbv, std::vector<std::uint64_t>());
    out.popcount.assign(nbv, 0);
    out.inv3f.assign(nbv, 0);
    // std::vector has no uninitialized resize, so the nbv vectors are
    // zero-filled concurrently, one thread each.
    std::vector<std::thread> fill;
    fill.reserve(nbv);
    for (unsigned i = 0; i < nbv; ++i) {
        popc[i].store(0);
        out.inv3f[i] = Filter::inverse_pow3_mod2b(fmin + i, B);
    }
    unsigned spawned = 0;
    try {
        for (; spawned < nbv; ++spawned) {
            const unsigned i = spawned;
            fill.emplace_back([&out, i, words]() { out.bvs[i].resize(words); });
        }
    } catch (const std::system_error&) { // NOLINT(bugprone-empty-catch): see below
        // After a failed spawn this thread fills the rest below.
    }
    for (std::thread& th : fill) {
        th.join();
    }
    for (unsigned i = spawned; i < nbv; ++i) {
        out.bvs[i].resize(words);
    }
}

// Packs the bitvectors from the class-major table. A unit is (level i, tile of
// G words of jl); tiles write disjoint words, so no two units touch the same
// word.
void pack_tiles(BuiltLevel& out,
                std::vector<std::atomic<std::uint64_t>>& popc,
                const std::unique_ptr<std::uint16_t[]>& cm,
                const std::vector<std::int64_t>& thr,
                unsigned B,
                unsigned H,
                unsigned nbv,
                unsigned fmin,
                unsigned nthreads) {
    const unsigned L = B - H;
    const std::uint64_t span = std::uint64_t{1} << B;
    const std::uint64_t classes = std::uint64_t{1} << L;
    const std::uint64_t hspan = std::uint64_t{1} << H;
    const std::uint64_t b_mask = span - 1u;
    const std::uint64_t hmask = hspan - 1u;
    const std::uint64_t lmask = classes - 1u;
    const std::uint64_t row_words = classes >> 6u; // words per jh row (L >= 6).
    const std::uint64_t G = std::min<std::uint64_t>(16u, row_words);
    const std::uint64_t tiles = row_words / G;
    run_chunked(
        nthreads,
        static_cast<std::uint64_t>(nbv) * tiles,
        4,
        [&](std::uint64_t lo, std::uint64_t hi) {
            std::vector<std::uint64_t> acc(static_cast<std::size_t>(G * hspan));
            std::vector<std::uint16_t> perm(static_cast<std::size_t>(hspan)); // jh*s mod 2^H.
            unsigned perm_level = ~0u;
            for (std::uint64_t unit = lo; unit < hi; ++unit) {
                const auto i = static_cast<unsigned>(unit / tiles);
                const std::uint64_t tile = unit % tiles;
                const std::int64_t t_i = thr[i];
                const std::uint64_t s = lo64(pow3(fmin + i)) & b_mask; // 3^f mod 2^B.
                if (perm_level != i) {
                    for (std::uint64_t jh = 0; jh < hspan; ++jh) {
                        perm[jh] = static_cast<std::uint16_t>((jh * s) & hmask);
                    }
                    perm_level = i;
                }
                std::fill(acc.begin(), acc.end(), 0u);
                for (std::uint64_t gw = 0; gw < G; ++gw) {
                    std::uint64_t* a = acc.data() + gw * hspan;
                    const std::uint64_t jl0 = (tile * G + gw) * 64u;
                    // Eight residues per sweep: they share the perm[] and a[] traffic,
                    // and no loop-carried index chain limits the issue rate.
                    for (unsigned b = 0; b < 64u; b += 8u) {
                        const std::uint16_t* rw[8];
                        std::uint64_t h0[8];
                        for (unsigned q = 0; q < 8u; ++q) {
                            const std::uint64_t r0 = ((jl0 + b + q) * s) & b_mask;
                            rw[q] = cm.get() + ((r0 & lmask) << H);
                            h0[q] = r0 >> L;
                        }
                        for (std::uint64_t jh = 0; jh < hspan; ++jh) {
                            const std::uint64_t pj = perm[jh];
                            const auto bit = [&](unsigned q) {
                                return static_cast<std::uint64_t>(
                                    static_cast<std::int64_t>(rw[q][(h0[q] + pj) & hmask]) < t_i);
                            };
                            a[jh] |=
                                (bit(0) | (bit(1) << 1u) | (bit(2) << 2u) | (bit(3) << 3u) |
                                 (bit(4) << 4u) | (bit(5) << 5u) | (bit(6) << 6u) | (bit(7) << 7u))
                                << b;
                        }
                    }
                }
                std::uint64_t* bv = out.bvs[i].data();
                std::uint64_t pc = 0;
                for (std::uint64_t jh = 0; jh < hspan; ++jh) {
                    for (std::uint64_t gw = 0; gw < G; ++gw) {
                        const std::uint64_t w = acc[gw * hspan + jh];
                        bv[jh * row_words + tile * G + gw] = w;
                        pc += static_cast<std::uint64_t>(std::popcount(w));
                    }
                }
                popc[i].fetch_add(pc, std::memory_order_relaxed);
            }
        });
}

// Bit j of BV_i is residue j * 3^{fmin+i} mod 2^B, so walking j
// increments the residue by 3^f (mod 2^B). Each bitvector i is independent
// (writes only its own bvs[i]/popcount[i]/inv3f[i]), so the nbv vectors are
// packed in parallel across the same thread budget. The bit is set ("must
// still be checked") iff dip(residue) < t_i.
// For B >= 6 each 64-bit word is built in a register as a branch-free OR of its
// 64 must-check bits, then stored and counted once. Bit j of word w is residue
// index 64w + j, visited in the same +3^f order as the per-bit loop that packs
// B < 6.
void pack_level(BuiltLevel& out,
                const std::vector<std::int64_t>& thr,
                const std::vector<std::uint16_t>& dip_table,
                unsigned B,
                unsigned fmin,
                unsigned nbv,
                unsigned threads) {
    const std::uint64_t span = std::uint64_t{1} << B;
    const std::uint64_t b_mask = span - 1;
    // ceil(2^B / 64): B < 6 packs its 2^B bits into one (sub-)word. For B >= 6
    // this is 2^B / 64, the layout the cache files use.
    const std::uint64_t words = (span + 63u) / 64u;
    out.bvs.assign(nbv, std::vector<std::uint64_t>(words, 0));
    out.popcount.assign(nbv, 0);
    out.inv3f.assign(nbv, 0);
    unsigned nthreads = threads == 0 ? 1u : threads;
    nthreads = std::min<unsigned>(nthreads, nbv == 0 ? 1u : nbv);
    std::atomic<unsigned> next{0};
    const auto pack_one = [&](unsigned i) {
        const unsigned f = fmin + i;
        const std::int64_t t_i = thr[i];
        const std::uint64_t step = lo64(pow3(f)) & b_mask; // 3^f mod 2^B.
        out.inv3f[i] = Filter::inverse_pow3_mod2b(f, B);
        std::vector<std::uint64_t>& bv = out.bvs[i];
        std::uint64_t popc = 0;
        std::uint64_t residue = 0;
        if (span >= 64) {
            for (std::uint64_t w = 0; w < words; ++w) {
                std::uint64_t word = 0;
                for (unsigned b = 0; b < 64u; ++b) {
                    const bool must_check = static_cast<std::int64_t>(dip_table[residue]) < t_i;
                    word |= static_cast<std::uint64_t>(must_check) << b;
                    residue = (residue + step) & b_mask;
                }
                bv[w] = word;
                popc += static_cast<std::uint64_t>(std::popcount(word));
            }
            out.popcount[i] = popc;
            return;
        }
        for (std::uint64_t j = 0; j < span; ++j) {
            const bool must_check = static_cast<std::int64_t>(dip_table[residue]) < t_i;
            if (must_check) {
                bv[j >> 6] |= (std::uint64_t{1} << (j & 63u));
                ++popc;
            }
            residue = (residue + step) & b_mask;
        }
        if (span < 64u) {
            // B < 6: replicate the 2^B-periodic pattern across the whole
            // word. The sub-word read (read_window) wraps the 2^B boundary
            // only once per 64-bit window, so when 2^A > 2^B a candidate a
            // past the first wrap reads bit (j0 + a - 2^B) of this word;
            // replication makes that bit equal bit (j0 + a) mod 2^B. popcount
            // above still counts the 2^B distinct residues only.
            std::uint64_t w = bv[0];
            for (std::uint64_t s = span; s < 64u; s <<= 1u) {
                w |= w << s;
            }
            bv[0] = w;
        }
        out.popcount[i] = popc;
    };
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.emplace_back([&]() {
            for (unsigned i = next.fetch_add(1); i < nbv; i = next.fetch_add(1)) {
                pack_one(i);
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
}

} // namespace collatz::step2::detail
