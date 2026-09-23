// Step 2 Filter construction: the bitvector cache lookup, the lookahead level
// build on a miss, the mod-9 candidate masks, and the window tables the dip walk
// folds in.

#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/step2_cache.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace collatz::step2 {

using namespace detail;

void Filter::build_window_tables() {
    // Window chain-merge table for t = window_chains.
    if (params_.window_chains != 0 && !window_chain_table_) {
        assert(params_.window_chains <= preimage::max_t &&
               "window_chains must be <= preimage::max_t");
        window_chain_table_ =
            std::make_shared<const preimage::WindowChainTable>(params_.window_chains);
    }
    // Merge-template table: the same WindowChainTable with an odd cap
    // t = min(D, max_window_t) and a backward-preimage BFS depth D (= the max
    // ancestor word length i+j<=D). Folded on top of the window-chain table in
    // the dip walk, so dip is monotone (dip_D >= dip_{t_w}).
    if (params_.window_templates_D != 0 && !window_template_table_) {
        const unsigned tcap =
            std::min<unsigned>(params_.window_templates_D, preimage::max_window_t);
        window_template_table_ =
            std::make_shared<const preimage::WindowChainTable>(tcap, params_.window_templates_D);
    }
}

void Filter::bind_owned_views() {
    bv_.resize(bvs_.size());
    for (std::size_t i = 0; i < bvs_.size(); ++i) {
        bv_[i] = std::span<const std::uint64_t>(bvs_[i]);
    }
}

Filter::Filter(const Params& params) : params_(params) {
    assert(params_.A <= max_A && "A must be <= max_A (64-bit candidate mask)");
    assert(params_.B >= 2 && params_.B <= max_B && "B must be in [2, max_B]");
    assert(params_.N > params_.A && "N must exceed A");

    partial_oee_ = params_.partial_oee;
    // The window tables (window_chains, window_templates_D) are read only by
    // the dip walk inside build_level (a bitvector cache miss). The D=12
    // template table is a depth-12 backward-preimage BFS over 3^12 classes, so
    // construction waits until it is known to be needed (lazy build below) and a
    // warm cache-hit run pays nothing for it.

    m_bits_ = params_.N - params_.A;
    // fmin = ceil((306 M + 1)/485) = (306 M + 485) / 485 (integer division).
    const std::uint64_t numer =
        kHalveWeight * static_cast<std::uint64_t>(m_bits_) + static_cast<std::uint64_t>(kOddWeight);
    fmin_ = static_cast<unsigned>(numer / static_cast<std::uint64_t>(kOddWeight));

    b_mask_ = (std::uint64_t{1} << params_.B) - 1;
    twoA_ = 1u << params_.A;
    words_ = (twoA_ + 63u) / 64u; // 1 for A <= 6, 16 at A = 10.

    // The depth-B lookahead level. With use_bv_cache set, the on-disk
    // packed-bitvector cache keyed by (B, m_bits, num_bitvectors, fmin, dip tag)
    // is tried first. A validated hit skips the O(2^B) dip walk and the packing; a
    // miss builds in-process (the dip walk may itself load the dip cache) and
    // writes the packed vectors back atomically. The mask is the same either way:
    // the cache stores exactly what build_level produces.
    std::string cache_dir;
    if (params_.use_bv_cache) {
        cache_dir = cache::resolve_dir();
    }
    // The dip/bv content depends on both window_chains (t_w) and
    // window_templates_D (D). One cache tag encodes both (window_chains < 32,
    // window_templates_D <= 12) so the two parameters never collide on disk; with
    // D = 0 the tag is t_w alone.
    const unsigned dip_tag = params_.window_chains + 32u * params_.window_templates_D;
    const bool bv_cacheable = params_.use_bv_cache && !cache_dir.empty();
    bool loaded = false;
    if (bv_cacheable) {
        const cache::BvKey key{params_.B, m_bits_, params_.num_bitvectors, fmin_, dip_tag};
        // A zero-copy mmap of the plain file: spans in place, no 2 GiB copy and
        // zero-fill. map_bitvectors returns ok=false on any miss or mapping
        // failure; the tables are then built below, or refused under cache_only.
        cache::MappedBitvectors mapped = cache::map_bitvectors(cache_dir, key);
        if (mapped.ok) {
            bv_mapping_ = std::move(mapped.keepalive);
            inv3f_ = std::move(mapped.inv3f);
            bv_popcount_ = std::move(mapped.popcount);
            bv_.resize(mapped.bv.size());
            for (std::size_t i = 0; i < mapped.bv.size(); ++i) {
                bv_[i] = std::span<const std::uint64_t>(mapped.bv[i], mapped.words);
            }
            loaded = true;
        }
        bv_cache_hit_ = loaded;
    }
    if (!loaded && params_.cache_only) {
        const std::uint64_t dip_mib = (std::uint64_t{2} << params_.B) >> 20;
        const std::string dip_size = dip_mib >= 1024 ? std::to_string(dip_mib >> 10) + " GiB"
                                                     : std::to_string(dip_mib) + " MiB";
        throw std::runtime_error(
            "step2: the Step-2 tables (B=" + std::to_string(params_.B) +
            ", N-A=" + std::to_string(m_bits_) + ", " + std::to_string(params_.num_bitvectors) +
            " bitvectors) are not in the cache" +
            (bv_cacheable ? " " + cache_dir : std::string(" (cache off)")) +
            ", and building them was not allowed (it needs a " + dip_size + " dip table)");
    }
    if (!loaded) {
        // On a cache miss build_level runs, and its dip walk needs the window
        // tables; a cache hit never builds them.
        build_window_tables();
        // The dip walk can still load the dip cache (keyed by B, tw).
        BuiltLevel fine = build_level(params_.B,
                                      fmin_,
                                      m_bits_,
                                      params_.num_bitvectors,
                                      params_.threads,
                                      /*cache_dir=*/cache_dir,
                                      /*wct=*/window_chain_table_.get(),
                                      /*tw=*/dip_tag,
                                      /*wtt=*/window_template_table_.get());
        bvs_ = std::move(fine.bvs);
        bv_popcount_ = std::move(fine.popcount);
        inv3f_ = std::move(fine.inv3f);
        bind_owned_views(); // bv_ points into the owned bvs_.
        if (bv_cacheable) {
            const cache::BvKey key{params_.B, m_bits_, params_.num_bitvectors, fmin_, dip_tag};
            cache::store_bitvectors(cache_dir, key, bvs_, inv3f_, bv_popcount_); // best effort
        }
    }

    // mod-9 masks. 2^{N-A} mod 9 by repeated doubling.
    unsigned p = 1;
    for (unsigned s = 0; s < m_bits_; ++s) {
        p = (p * 2u) % 9u;
    }
    pow2m_mod9_ = p;
    for (unsigned r0 = 0; r0 < 9; ++r0) {
        for (unsigned r2 = 0; r2 < 9; ++r2) {
            CandidateMask cm;
            cm.words = words_;
            cm.zero(); // OR-populated below; base must be clear (not auto-zeroed).
            for (std::uint32_t a = 0; a < twoA_; ++a) {
                const unsigned r = (r0 + a * r2) % 9u;
                const bool excluded = (r == 2 || r == 4 || r == 5 || r == 8);
                if (!excluded) {
                    cm.w[a >> 6] |= (std::uint64_t{1} << (a & 63u));
                }
            }
            m9_[r0][r2] = cm;
        }
    }
}

} // namespace collatz::step2
