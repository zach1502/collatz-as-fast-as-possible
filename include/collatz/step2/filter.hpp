#ifndef COLLATZ_STEP2_FILTER_HPP
#define COLLATZ_STEP2_FILTER_HPP

// The Step-2 lookahead sieve Filter (collatz/step2.hpp includes this file).

#include "collatz/step1.hpp"
#include "collatz/step2/candidate_mask.hpp"
#include "collatz/step2/params.hpp"
#include "collatz/u128.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace collatz::preimage {
class WindowChainTable; // Window chain-merge dip terms (see preimage_chain/window_chain_table.hpp).
} // namespace collatz::preimage

namespace collatz::step2 {

/// A prebuilt Step-2 lookahead sieve for one (N, A, B, num_bitvectors) tuple.
///
/// Construction builds dip_B for all 2^B residues (in parallel) and packs the
/// bitvectors; afterwards mask() is a cheap per-survivor lookup. Move-only and
/// heavy (on a warm cache hit it may hold a live read-only mmap of the packed
/// bitvectors and expose spans into it, so copying is disabled); typically
/// built once and shared const.
class Filter {
public:
    /// Builds the filter. Asserts A <= max_A and B <= max_B, requires N > A, and
    /// builds dip with Params::threads (clamped to >= 1).
    explicit Filter(const Params& params);

    // Move-only: the read views (bv_) point into either the owned bitvectors or
    // the live cache mmap held by bv_mapping_; a deep copy would alias the
    // source's storage, so copying is disabled. Moving preserves the backing
    // addresses (vector move keeps inner-vector data pointers; the mmap address
    // is fixed), so the defaulted moves keep the spans valid.
    Filter(Filter&&) noexcept = default;
    Filter& operator=(Filter&&) noexcept = default;
    Filter(const Filter&) = delete;
    Filter& operator=(const Filter&) = delete;

    /// Bitmask over the 2^A candidates of a Step-1 survivor: bit a is set iff
    /// n = n0 + a*2^{N-A} must still be checked (i.e. survived the lookahead
    /// and mod-9 sieves). Returns a CandidateMask whose `words` count matches A
    /// (1 word for A <= 6, up to 256 for A == 14; 16 for the A=10 default).
    [[nodiscard]] CandidateMask mask(const step1::Node& survivor) const;

    /// Burst window touch: mask()'s window read spans words idx ..
    /// idx + words_ (17 words = 136 B at A=10: always two 128-B lines).
    /// Demand-loads the first and the last word (plus idx + 8 when words_ > 8,
    /// for 64-B-line hosts) and returns their sum, which the caller keeps live
    /// and discards (a demand load walks a TLB miss that a prefetch hint may
    /// drop). In-bounds reads only (indices wrap like mask()'s); never affects a
    /// mask.
    std::uint64_t touch2(const step1::Node& survivor) const noexcept;

    /// Smallest f with a positive threshold, fmin = ceil((306(N-A)+1)/485) (see
    /// collatz/step2.hpp).
    [[nodiscard]] unsigned fmin() const noexcept {
        return fmin_;
    }

    /// Cache telemetry: true iff the packed bitvectors were loaded from the
    /// on-disk cache (so the O(2^B) dip walk + packing were skipped). It never
    /// affects the mask.
    [[nodiscard]] bool bv_cache_hit() const noexcept {
        return bv_cache_hit_;
    }

    /// The packed fine (depth-B) lookahead bitvectors: bvs()[i] is the 2^B-bit
    /// vector for f = fmin + i, as ceil(2^B / 64) little-endian uint64 words
    /// (bit j set iff residue j*3^{fmin+i} mod 2^B must still be checked).
    /// Exposed so the GPU survivor kernel can upload the same tables the CPU
    /// mask() reads; the mask value is identical either way. Each element is a
    /// span: a view into either the owned storage or the live cache mmap.
    [[nodiscard]] const std::vector<std::span<const std::uint64_t>>& bitvectors() const noexcept {
        return bv_;
    }

    /// inv(3^{fmin+i}) mod 2^B for bitvector i (the multiplier the window start
    /// j0 = (m mod 2^B) * inv3f is built from). Companion to bitvectors().
    [[nodiscard]] std::uint64_t inv3f(unsigned i) const noexcept {
        return inv3f_[i];
    }

    /// 2^{N-A} mod 9, the fixed column selector of the mod-9 candidate masks.
    [[nodiscard]] unsigned pow2m_mod9() const noexcept {
        return pow2m_mod9_;
    }

    // Static primitives, exposed for tests.

    /// dip_B(m): max of S_B(m) as defined in collatz/step2.hpp, with the window
    /// chain table `wct` (odd cap t_w) and the deeper window-template table
    /// `wtt` (odd cap min(D, max_window_t)) folded in; either may be null, and
    /// with both null this is the plain dip_B. m is taken mod 2^B. Requires
    /// 2 <= B <= max_B. Uses the parity-only path-merge/OEE criteria, which agree
    /// with arithmetic for the actual value m. With a table, after each window
    /// step k (value v_k, f_k odd steps) it also considers the term
    /// A_k + gain(v_k mod 3^t, min(f_k, t)), the max over the non-null tables:
    /// the best admissible decreasing preimage chain of v_k's class, valid by
    /// word-determinism because v <= f_k. dip is monotone non-decreasing as
    /// tables are added. See the merge-template note in
    /// preimage_chain/window_chain_table.hpp.
    [[nodiscard]] static std::uint32_t dip(std::uint64_t m,
                                           unsigned B,
                                           const preimage::WindowChainTable* wct,
                                           const preimage::WindowChainTable* wtt);

    /// Parity-only criterion for "T^k(x) == 2 (mod 3)" (Lemma 2.8). `parity_bits`
    /// has bit s set iff step s (0-based) took the odd branch. Requires at least
    /// one odd step among steps [0, k); returns false otherwise (the arithmetic
    /// mod-3 is then not determined by parities). True iff an even number of
    /// even steps have elapsed since the last odd step.
    [[nodiscard]] static bool parity_mod3_is_2(std::uint64_t parity_bits, unsigned k) noexcept;

    /// Modular inverse of 3^f modulo 2^B (3^f is odd, so it is invertible),
    /// computed by Newton iteration. Result reduced to [0, 2^B). Requires
    /// f <= 80 (POW3 table bound) and 1 <= B <= 64.
    [[nodiscard]] static std::uint64_t inverse_pow3_mod2b(unsigned f, unsigned B) noexcept;

    /// n mod 9 via the fold 2^64 == 7 (mod 9): (lo64 + 7*hi64) mod 9.
    [[nodiscard]] static unsigned mod9(u128 n) noexcept;

private:
    Params params_;
    unsigned m_bits_;      ///< M = N - A.
    unsigned fmin_;        ///< ceil((306 M + 1)/485).
    std::uint64_t b_mask_; ///< 2^B - 1.
    std::uint32_t twoA_;   ///< 2^A (number of candidates), <= 16384.
    unsigned words_;       ///< Words per CandidateMask = ceil(2^A / 64), 1..256.

    /// One packed bitvector per i in [0, num_bitvectors): 2^B bits. Owned storage,
    /// populated on a cache miss (built in-process); empty on a cache hit, where
    /// the bytes live in the mapping (bv_mapping_) instead.
    std::vector<std::vector<std::uint64_t>> bvs_;
    /// Read views: bv_[i] is the span mask()/the GPU read, over either bvs_[i]
    /// (owned) or the mapped payload (zero-copy). Bound once in the constructor.
    std::vector<std::span<const std::uint64_t>> bv_;
    /// Keeps the read-only cache mmap alive when bv_ views point into it
    /// (zero-copy hit); null on the owned path. Opaque handle owned by the cache
    /// layer (collatz::step2::cache::MappedBitvectors::keepalive).
    std::shared_ptr<void> bv_mapping_;
    std::vector<std::uint64_t> bv_popcount_; ///< set-bit count per bitvector.
    std::vector<std::uint64_t> inv3f_;       ///< inv(3^{fmin+i}) mod 2^B.

    /// mod-9 candidate masks, indexed [n0 mod 9][2^{N-A} mod 9]: bit a set iff
    /// (n0 + a*2^{N-A}) mod 9 is not in {2,4,5,8}. Each is a full CandidateMask
    /// (up to 256 words) precomputed once for the filter's A.
    std::array<std::array<CandidateMask, 9>, 9> m9_{};
    unsigned pow2m_mod9_ = 1; ///< 2^{N-A} mod 9.

    bool partial_oee_ = false; ///< High-bit partial OEE pruning.

    /// Window chain-merge table for t = params_.window_chains (null when 0).
    /// Consumed only during construction (the dip walk); shared and immutable.
    std::shared_ptr<const preimage::WindowChainTable> window_chain_table_;

    /// Window-template table for D = params_.window_templates_D, null when
    /// D == 0: a deeper WindowChainTable with t = min(D, max_window_t) and BFS
    /// depth D, folded on top of the window chain table. Consumed only during
    /// construction (the dip walk); shared and immutable.
    std::shared_ptr<const preimage::WindowChainTable> window_template_table_;

    /// Builds window_chain_table_ and window_template_table_ from params_ when
    /// enabled; a no-op once they exist. Called only when a dip walk runs (a
    /// bitvector cache miss), so a cache hit skips the table build, which is
    /// costly at D=12.
    void build_window_tables();

    /// Points bv_ at the owned bvs_ (one span per bitvector). Called on the
    /// build (cache miss) path; a cache hit binds bv_ to the mmap directly
    /// instead.
    void bind_owned_views();

    /// Cache telemetry (see bv_cache_hit()).
    bool bv_cache_hit_ = false;
};

} // namespace collatz::step2

#endif // COLLATZ_STEP2_FILTER_HPP
