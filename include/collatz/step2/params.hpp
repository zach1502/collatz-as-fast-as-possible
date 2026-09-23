#ifndef COLLATZ_STEP2_PARAMS_HPP
#define COLLATZ_STEP2_PARAMS_HPP

// Step-2 filter configuration and limits (collatz/step2.hpp includes this file).

namespace collatz::step2 {

/// Configuration for a Step-2 filter.
struct Params {
    unsigned N;                  ///< Verify every n < 2^N.
    unsigned A = 6;              ///< High bits added past Step 1 (must be <= max_A).
    unsigned B = 24;             ///< Lookahead depth (must be <= max_B).
    unsigned num_bitvectors = 8; ///< Number of per-f bitvectors to build.
    unsigned threads = 1;        ///< Worker threads used to build the filter.
    /// High-bit partial Odd-Even-Even pruning. When true, mask() ANDs in the
    /// extra "keep a < a*" prefix mask. mask() reads a* from
    /// step1::Node::oee_astar (the DFS-carried value), so the survivor must come
    /// from an enumeration run with step1::Options::track_oee_astar set (as
    /// verify::run does). A non-tracked survivor carries kOeeAStarNone and is
    /// (soundly) under-pruned.
    bool partial_oee = false;
    /// Use the on-disk bitvector/dip cache (collatz/step2_cache.hpp). Off by
    /// default so unit tests that build small filters never touch the shared
    /// cache directory; the CLI/verifier turns it on (`verify::Options::bv_cache`,
    /// `--no-bv-cache` to disable). When on, the packed bitvectors are loaded
    /// from (or, on a miss, built and written to) a file keyed by (B, N-A,
    /// num_bitvectors); the dip walk is additionally cached keyed by B alone.
    /// A hit and a miss give bit-identical masks.
    bool use_bv_cache = false;
    /// Window chain-merge dip terms. 0 = off (the Params default; the CLI default
    /// is 6). When t_w in [1, max_t] the dip walk folds the mod-3^t
    /// preimage-chain certificates into the window dip at every step k: at value
    /// v_k with f_k odd steps it adds the term
    /// A_k + gain(v_k mod 3^{t_w}, min(f_k, t_w)) using the best admissible
    /// decreasing chain of that class (see preimage::WindowChainTable). The
    /// terms cost no per-survivor memory or time: only the dip precomputation
    /// changes, and the cached bitvectors prune more. The dip/bv on-disk caches
    /// include t_w in their key. Sound: every added term is a merge below n by
    /// the rational-bound argument of descent/PM/OEE, and dip is monotone
    /// non-decreasing in t_w.
    unsigned window_chains = 0;
    /// General merge-template window dip terms. 0 = off (the Params default; the
    /// CLI default is 12). When D in [1, max_window_t] the Step-2 dip
    /// additionally folds in the general (u, w)
    /// merge templates with ancestor word length i <= D. As derived in
    /// preimage_chain/window_chain_table.hpp, a (u, w) template's dip term equals
    /// an empty-w ancestor chain of odd-depth o_u <= D applied at a later window
    /// step, so the fold reduces to a deeper WindowChainTable
    /// (t = min(D, max_window_t), BFS depth D). It is applied in addition to
    /// window_chains, so dip is monotone non-decreasing (dip_D >= dip_{t_w}) and
    /// the D=0 path is exactly the window_chains dip. As with window_chains, only
    /// the dip precomputation changes; the dip/bv cache key encodes D. Sound by
    /// the window_chains argument at the deeper cap.
    unsigned window_templates_D = 0;
    /// With use_bv_cache: when the packed bitvectors are not in the cache, the
    /// constructor throws std::runtime_error instead of building them (the
    /// build needs a 2^B-entry uint16 dip table, 8 GiB at B=32).
    bool cache_only = false;
};

/// The largest A supported. The candidate mask is a fixed-capacity bit array of
/// up to CandidateMask::kMaxWords words; A == 14 (2^14 = 16384 bits = 256 words,
/// 2 KB) is the widest. A <= 6 stays a single-word fast path and A <= 10 the
/// 16-word path; A in {11,12,13,14} use up to 256 words. The paper uses
/// A=6 (CPU-optimal) and A=10 (GPU-optimal).
inline constexpr unsigned max_A = 14;

/// The largest lookahead depth supported. B=30 => 2^30 bits = 128 MB per
/// bitvector (8 => 1 GB); a build at B=32 needs the 2^32-entry uint16 dip
/// table (8 GiB). dip's T^k(m) < 3^B (< 2^51 at B=32) stays exact in 64-bit;
/// dip_B <= 306*B = 9792 at B=32 fits a uint16 (the cache dip array). All 2^B
/// index arithmetic is 64-bit: a 32-bit worker-count clamp would truncate
/// span = 2^32 to 0 and leave the B=32 dip table all-zero.
inline constexpr unsigned max_B = 32;

} // namespace collatz::step2

#endif // COLLATZ_STEP2_PARAMS_HPP
