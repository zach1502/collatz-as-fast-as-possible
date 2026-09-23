// Step 2 lookahead level build: build_level takes the class-major cold build or
// the natural-order path (the dip table, then the packer).

#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace collatz::step2 {

using namespace detail;

namespace {

// The per-f thresholds t_i = 485*(fmin+i) - 306*m_bits (strictly increasing
// in i; t_0 >= 1 by fmin's definition). A residue r "must still be checked"
// for bitvector i iff dip(r) < t_i.
std::vector<std::int64_t> level_thresholds(unsigned fmin, unsigned m_bits, unsigned nbv) {
    std::vector<std::int64_t> thr(nbv, 0);
    for (unsigned i = 0; i < nbv; ++i) {
        thr[i] = kOddWeight * static_cast<std::int64_t>(fmin + i) -
                 kHalveWeight * static_cast<std::int64_t>(m_bits);
    }
    return thr;
}

// The natural-order dip table, one Filter::dip walk per residue, written to the
// dip cache when `cache_dir` is set (best effort).
void build_dip_table(std::vector<std::uint16_t>& dip_table,
                     unsigned B,
                     unsigned threads,
                     const std::string& cache_dir,
                     unsigned tw,
                     const preimage::WindowChainTable* wct,
                     const preimage::WindowChainTable* wtt) {
    const std::uint64_t span = std::uint64_t{1} << B;
    dip_table.resize(span);
    const unsigned nthreads = clamp_threads(threads, span);
    const std::uint64_t chunk = (span + nthreads - 1) / nthreads;
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) {
        const std::uint64_t lo = static_cast<std::uint64_t>(t) * chunk;
        const std::uint64_t hi = std::min(lo + chunk, span);
        if (lo >= hi) {
            break;
        }
        pool.emplace_back([&dip_table, lo, hi, B, wct, wtt]() {
            for (std::uint64_t mm = lo; mm < hi; ++mm) {
                dip_table[mm] = static_cast<std::uint16_t>(Filter::dip(mm, B, wct, wtt));
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    if (!cache_dir.empty()) {
        cache::store_dip(cache_dir, B, tw, dip_table); // best effort
    }
}

} // namespace

namespace detail {

/// Builds the bitvectors for lookahead depth B. Bit j of BV_i is set ("must
/// still be checked") iff dip_B(j*3^{fmin+i} mod 2^B) < t_i, where
/// t_i = 485*(fmin+i) - 306*m_bits. The 2^B dip table is computed in parallel
/// across `threads` workers, then each bitvector is packed by walking residues
/// in +3^f steps.
BuiltLevel build_level(unsigned B,
                       unsigned fmin,
                       unsigned m_bits,
                       unsigned nbv,
                       unsigned threads,
                       const std::string& cache_dir,
                       const preimage::WindowChainTable* wct,
                       unsigned tw,
                       const preimage::WindowChainTable* wtt) {
    const std::vector<std::int64_t> thr = level_thresholds(fmin, m_bits, nbv);

    // Class-major cold build. It walks a shared prefix once per class, so it
    // needs the window gain to be residue-local (gain(c, o) a function of c mod
    // 3^o; see WindowChainTable::residue_local). That is proved for every table
    // and re-checked exhaustively at table build; if a table fails it, the
    // natural-order path below runs.
    const bool gain_local =
        (wct == nullptr || wct->residue_local()) && (wtt == nullptr || wtt->residue_local());
    if (gain_local) {
        BuiltLevel cold;
        if (build_level_classmajor(cold, B, nbv, threads, thr, fmin, cache_dir, wct, tw, wtt)) {
            return cold;
        }
    }
    // The natural-order path packs from the 2^B uint16 dip table, loaded from
    // the dip cache or built by the per-residue walk (and then written to the
    // dip cache).
    bool dip_hit = false;
    std::vector<std::uint16_t> dip_table;
    if (!cache_dir.empty()) {
        dip_hit = cache::load_dip(cache_dir, B, tw, dip_table);
    }
    if (!dip_hit) {
        build_dip_table(dip_table, B, threads, cache_dir, tw, wct, wtt);
    }

    BuiltLevel out;
    pack_level(out, thr, dip_table, B, fmin, nbv, threads);
    return out;
}

} // namespace detail

} // namespace collatz::step2
