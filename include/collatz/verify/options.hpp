#ifndef COLLATZ_VERIFY_OPTIONS_HPP
#define COLLATZ_VERIFY_OPTIONS_HPP

// verify::Options, the parameters of a verification run.

#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

#include <cstddef>
#include <string>

namespace collatz::verify {

/// Default cap on Result::records (Options::records_cap).
inline constexpr std::size_t kRecordsCap = 1000000;

/// Verification parameters.
struct Options {
    unsigned N = 0; ///< Verify every n < 2^N (block mode: the depth K).
    /// Frontier block mode: verify the block
    /// [block_h * 2^N, (block_h + 1) * 2^N), assuming every smaller number
    /// converges. N keeps its depth role (M = N - A, B, unwalked_depth); the
    /// value width is value_bits(). 0 (the default) is the full run [0, 2^N).
    u128 block_h = 0;
    /// Report every walked candidate whose excursion exceeds this value
    /// (decimal) in Result::records. Empty (the default) turns the list off.
    /// The CLI fills in default_record_threshold() when none is given.
    std::string record_threshold;
    /// Where the block's hypothesis "every n < block start converges" comes
    /// from; written to the JSON as params.assumption_source. Empty (the
    /// default) resolves through assumption_source(). Never changes a result.
    std::string assumption_source;
    /// Most entries Result::records keeps: the records_cap smallest n, with
    /// Result::records_truncated set when more qualify. Tests lower it.
    std::size_t records_cap = kRecordsCap;
    unsigned A = 10;             ///< High bits for Step 2 (0 disables Step 2; needs N > A).
    unsigned B = 24;             ///< Step 2 look-ahead width (used when A > 0).
    unsigned num_bitvectors = 8; ///< Step 2 bitvector count (used when A > 0).
    unsigned threads = 1;        ///< Worker threads (0 => hardware_threads()).
    step1::Options step1{};      ///< Step 1 options (mod-3^t sieve, case slice).
    /// High-bit partial odd-even-even pruning in Step 2. The CLI turns it on
    /// (--no-partial-oee turns it off); the library keeps it off so the count
    /// and excursion test fixtures keep their values.
    bool partial_oee = false;
    /// Window chain-merge dip terms with odd-step cap t_w; flows into
    /// step2::Params::window_chains. 0 = off (the library default; the CLI
    /// default is 6). For t_w in [1, max_t] the Step-2 dip precomputation folds
    /// the mod-3^t preimage-chain certificates into the window dip at every step,
    /// which makes the bitvectors prune more at no per-survivor cost. Sound, and
    /// dip never decreases as t_w grows.
    unsigned window_chains = 0;
    /// General (u, w) merge-template dip terms with ancestor word length
    /// i + j <= D; flows into step2::Params::window_templates_D. 0 = off (the
    /// library default; the CLI default is 12). D in [1, max_window_t] adds them
    /// on top of window_chains, so dip_D >= dip_{t_w}. The cost is in the table
    /// build only.
    unsigned window_templates_D = 0;
    /// Use the on-disk Step-2 bitvector and dip cache (collatz/step2_cache.hpp);
    /// --no-bv-cache turns it off. The packed bitvectors are keyed by
    /// (B, N-A, num_bitvectors) and the dip table by B, so a warm run maps a file
    /// instead of rebuilding. Every result field is bit-identical on a hit or a
    /// miss. The cache directory is $COLLATZ_CACHE_DIR or %LOCALAPPDATA%\collatz.
    bool bv_cache = true;
};

} // namespace collatz::verify

#endif // COLLATZ_VERIFY_OPTIONS_HPP
