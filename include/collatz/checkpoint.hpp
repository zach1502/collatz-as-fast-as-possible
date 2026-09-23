#ifndef COLLATZ_CHECKPOINT_HPP
#define COLLATZ_CHECKPOINT_HPP

// Pause and resume of long verification runs (`collatz verify --checkpoint DIR`)
// and the exact merge of their slices (`collatz merge DIR...`).
//
// A run is split into K = 2^j disjoint case slices c/K (step1::Options case_c,
// case_k). Each finished slice is written atomically to
// DIR/slice_<c>_of_<K>.json (same format as `verify --json`, plus a
// "checkpoint" block), so killing the process loses at most the slice in
// progress. A restart skips every slice whose file parses, still matches its
// result_digest, carries the same run fingerprint and records the run's own
// parameters and commit, and redoes the others; once all K are present the
// slices are merged exactly:
// sums, wrapping u64, mod 2^61-1, (K-1)-fold preflight subtraction, max with
// smallest-n tie-break. Every one of these folds is commutative and
// associative, so the merged Result equals an unsharded run on every mergeable
// scalar.
//
// Uses only C++20 and std::filesystem, plus the CRT's _commit for the flush in
// checkpoint_io.cpp.

#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace collatz::checkpoint {

// Minimal JSON value and parser for the files this program writes. Numbers
// keep their raw text, so u64 values parse exactly.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    std::string text; ///< raw number text, or the decoded string
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    [[nodiscard]] const Json* get(std::string_view key) const;
};

/// Parses `src` as one JSON document. Returns nullopt and sets `err` on a
/// syntax error or trailing text, so a truncated file is rejected.
[[nodiscard]] std::optional<Json> parse_json(std::string_view src, std::string* err = nullptr);

// Metadata of the `verify --json` document, which `verify --json`, the
// checkpoint slices and merged.json share.
struct JsonMeta {
    std::string commit = "unknown";
    std::string backend = "cpu";
    bool use_gpu = false;
    /// Un-walked depth K written as params.unwalked_depth. nullopt (the default) =
    /// verify::unwalked_depth(opts), which is exact for a real run; a merged
    /// document whose Options were rebuilt from JSON passes the slices'
    /// recorded depth instead.
    std::optional<unsigned> unwalked_depth;
};

/// Renders the Result and params as the `verify --json` document. A non-empty
/// `extra_block` is a complete `"key": value` member appended to the top object.
[[nodiscard]] std::string result_json(const JsonMeta& meta,
                                      const verify::Options& opts,
                                      const verify::Result& r,
                                      const std::string& extra_block = {});

/// First line of result_digest_text, also written as result.result_digest_schema.
/// The version changes whenever a digest field changes meaning; in /2,
/// overflow_fallbacks counts only walks that passed 2^128-1 (see
/// verify::Result::overflow_fallbacks). server/src/result/digest.ts DIGEST_SCHEMA
/// must match.
inline constexpr const char* kResultDigestSchema = "collatz-result-digest/2";

/// Canonical text hashed into a work unit's "result_digest": one `key=value`
/// line per field in this fixed order, decimal integers, '\n' line ends, first
/// line kResultDigestSchema. Fields: block_h, K (= N), assumes_below
/// (the block start L = block_h * 2^K below which convergence is assumed),
/// case (c/k), A, B, bitvectors, kernel (table16), mod3t, partial_oee,
/// window_chains, window_templates_D, unwalked_depth, record_threshold ("-"
/// when off), candidates_checked, step2_masked_out, step1_survivors,
/// step3_steps_total, blocks_total, coverage_checksum, pow_checksum,
/// pow_kernel_id, max_excursion (exact, wide when above 2^128),
/// max_excursion_n, max_exceeds_u128, max_exact, overflow_fallbacks, preflight
/// (5 comma-separated values), records ("n:peak" joined by ';') and
/// records_truncated. Every field is independent of threads, slicing order and
/// machine; timings, threads, backend, commit, cache hits, prefix_walks and the
/// free-text assumption_source are left out.
[[nodiscard]] std::string
result_digest_text(const verify::Options& opts, unsigned unwalked_depth, const verify::Result& r);

/// Lowercase hex SHA-256 of result_digest_text.
[[nodiscard]] std::string
result_digest(const verify::Options& opts, unsigned unwalked_depth, const verify::Result& r);

// Run fingerprint: every option that can change a Result scalar (N, the block
// h, A, B, the sieve toggles, the record threshold, the GPU flag,
// result-affecting env knobs, the binary commit), plus the resolved assumption
// source (written into every slice's params). Threads and pure execution-shape
// knobs are excluded, so a run may be resumed with a different --threads.
using Fingerprint = std::map<std::string, std::string>;

[[nodiscard]] Fingerprint
make_fingerprint(const verify::Options& opts, bool use_gpu, const std::string& commit);

/// Human-readable list of keys whose values differ ("B: 30 (DIR) vs 31 (now)").
[[nodiscard]] std::vector<std::string> fingerprint_diff(const Fingerprint& stored,
                                                        const Fingerprint& now);

/// The COLLATZ_* environment names that can change a printed Result scalar
/// (feature switches, fault injection, alternative Step-2 or Step-3 algorithms
/// or kernels, and anything not clearly neutral); they are part of the
/// fingerprint. The other names the sources read change only scheduling,
/// sizing, caching, transfers or diagnostic output, which cannot change a
/// Result because every Result fold is order-independent, or are fingerprinted
/// through a resolved option. tests/core/test_checkpoint_fingerprint.cpp lists
/// them and fails if a name read in src/ or include/ is in neither list.
[[nodiscard]] const std::vector<const char*>& result_env_names();

/// Writes `content` to `path` through a temporary file in the same directory
/// and a rename. The file is flushed to disk (_commit) before the rename. A
/// crash leaves the previous content, the written content, or a stray
/// `<path>.tmp`; a failed write or rename removes `<path>.tmp` and leaves the
/// previous content.
[[nodiscard]] bool
write_file_atomic(const std::string& path, const std::string& content, std::string& err);

/// The largest slice count. `verify --checkpoint` splits a run into at most
/// this many slices (max_slices), so parse_slice refuses a larger case_k and
/// coverage() and merge() a larger K before they allocate anything sized by it.
inline constexpr std::uint64_t kMaxSlices = 1024;

struct Slice {
    std::uint64_t c = 0;
    std::uint64_t k = 1;
    unsigned N = 0;
    u128 block_h = 0;            ///< params.block_h
    unsigned unwalked_depth = 0; ///< params.unwalked_depth, the un-walked depth K
    /// The params block minus the per-slice fields (case_c, case_k, threads,
    /// gpu), as "key" -> raw value text. merge() requires it to be equal across
    /// slices.
    std::map<std::string, std::string> params;
    /// The run's options as params records them: N, block_h, A, B, bitvectors,
    /// mod3t, partial_oee, the window terms, the record threshold, the
    /// assumption source, threads, and case_c/case_k in step1. parse_slice
    /// recomputes the result digest from them; `collatz merge` writes its JSON
    /// with them.
    verify::Options opts;
    bool gpu = false;        ///< params.gpu
    std::string backend;     ///< the slice's "backend" (the backend that ran)
    std::string commit;      ///< the slice's "commit" (the binary that wrote it)
    Fingerprint fingerprint; ///< empty when the file has no checkpoint fingerprint
    verify::Result r;        ///< scalars + preflight_* + step1.survivors + seconds
    std::string source;      ///< file path (for messages)
};

/// Parses one slice JSON (a `verify --json` document, with or without the
/// checkpoint block). Returns false, with a reason in `err`, when a field is
/// missing, malformed or out of range (case_k not a power of two in
/// [1, kMaxSlices], a parameter above 2^32-1), when the file comes from a run
/// this build cannot merge (another kernel, window landing on, another
/// result_digest_schema), or when result.result_digest is not the digest of
/// the fields read (result_digest of `opts`, `unwalked_depth` and `r`), so a
/// file changed after it was written is refused. The members `collatz merge`
/// writes back (commit, backend, params.threads, gpu, assumption_source) are
/// required too, so none falls back to a default.
[[nodiscard]] bool parse_slice(std::string_view text, Slice& out, std::string& err);

/// parse_slice of a file; one that is not a regular file (after symlinks) or is
/// above 256 MiB, more than any slice this program writes, is refused unread.
[[nodiscard]] bool read_slice_file(const std::string& path, Slice& out, std::string& err);

/// Coverage check of a slice set against K: `missing` gets every c in [0, K)
/// with no slice, `duplicate` every c seen more than once. The lists are sized
/// by K, which may come from a file, so K outside [1, kMaxSlices] throws
/// std::invalid_argument before anything is allocated (merge() and `collatz
/// merge` check K first and report it instead).
void coverage(const std::vector<Slice>& slices,
              std::uint64_t K,
              std::vector<std::uint64_t>& missing,
              std::vector<std::uint64_t>& duplicate);

/// Exact k-way merge (k = slices.size(), which must equal every slice's K, a
/// power of two in [1, kMaxSlices], and cover [0, K) exactly once):
///   candidates_checked            = sum - (K-1)*pre              (exact)
///   step2_masked_out, step1_survivors, overflow_fallbacks = sum
///   step3_steps_total, blocks_total = (sum - (K-1)*pre) mod 2^64
///   coverage_checksum, pow_checksum = (sum - (K-1)*pre) mod 2^61-1
///   max_excursion(_n)             = max, smallest n on ties (exact, also
///                                   above 2^128 via max_excursion_wide)
///   max_exceeds_u128              = OR
///   records                       = union, ascending n (merge_records), the
///                                   kRecordsCap smallest n
///   records_truncated             = OR, or set when the union exceeds the cap
/// prefix_walks (order dependent diagnostic) and the seconds fields are summed
/// for display only. max_exact is recomputed from the merged record
/// (verify::result_max_exact, with the block bound for a block run), and is
/// false whenever some slice has max_exceeds_u128 without the exact
/// max_excursion_wide (its maximum is unknown and could beat the merged record).
/// The preflight_* fields of the output equal the (shared) slice preflight, so
/// the merged Result is itself a valid k=1 slice. Block slices carry no
/// preflight (all zero), so the subtraction is a no-op there.
/// Also refuses slices whose params differ (A, B, kernel, unwalked_depth, ...)
/// or that were written by different commits, with the differing keys listed,
/// and slices whose options verify would refuse (verify::validate_block), so a
/// crafted block_h cannot wrap the max_exact bound.
struct MergeInfo {
    unsigned depth = 0;  ///< the slices' unwalked_depth
    std::string backend; ///< the slices' backend, or "mixed: a + b" if they differ
    std::string commit;  ///< the slices' commit (all equal)
    /// Some slice has max_exceeds_u128 without max_excursion_wide: its maximum
    /// is unknown, so the merged max_exact is false.
    bool max_unknown = false;
};
[[nodiscard]] bool merge(const std::vector<Slice>& slices,
                         verify::Result& out,
                         std::string& err,
                         MergeInfo* info = nullptr);

/// Lower-level helpers, exposed for the property tests.
[[nodiscard]] std::uint64_t merge_mod_p(const std::vector<std::uint64_t>& parts,
                                        std::uint64_t pre); ///< (sum - (k-1)pre) mod 2^61-1
[[nodiscard]] std::uint64_t merge_wrap64(const std::vector<std::uint64_t>& parts,
                                         std::uint64_t pre); ///< (sum - (k-1)pre) mod 2^64
inline constexpr std::uint64_t kP61 = (std::uint64_t{1} << 61) - 1;

/// Default slice count: K = 2^(N-52) on the GPU and 2^(N-46) on the CPU,
/// clamped to [1, 1024] and to the case-split limit 2^(N-A-2).
[[nodiscard]] std::uint64_t default_slices(unsigned N, unsigned A, bool use_gpu) noexcept;

/// Largest K the case split supports at this N, A (2 + log2 K <= N - A).
[[nodiscard]] std::uint64_t max_slices(unsigned N, unsigned A) noexcept;

/// File name of slice c of K inside a checkpoint dir.
[[nodiscard]] std::string slice_file_name(std::uint64_t c, std::uint64_t K);

struct RunConfig {
    std::string dir;
    std::uint64_t K = 1;     ///< 0 = adopt from DIR/run.json, else default_slices
    bool K_explicit = false; ///< --slices given
    std::uint64_t lo = 0;    ///< --slice-range lo:hi (hi == 0 => K)
    std::uint64_t hi = 0;
    bool range_explicit = false;
    /// --checkpoint-force: discard DIR's previous run, or use a non-empty DIR
    /// without run.json (one this program did not make); only this program's
    /// file names are removed either way.
    bool force = false;
    verify::Options opts; ///< fully resolved options (case fields ignored)
    JsonMeta meta;        ///< backend label is overwritten per slice
    Fingerprint fingerprint;
    std::string command_line; ///< recorded in run.json for the resume hint
    /// Called once, after stale files (this program's *.tmp names, a leftover
    /// DIR/PAUSE) are removed and before the first slice; the CLI starts its
    /// PAUSE watcher here.
    std::function<void()> on_ready;
};

/// Runs one slice. `slice_opts` has step1.case_c/case_k set; the runner sets
/// `backend` to the backend that ran (e.g. "gpu", "cpu (fallback)").
using SliceRunner =
    std::function<verify::Result(const verify::Options& slice_opts, std::string& backend)>;

enum class Outcome {
    Complete,  ///< every slice present; `merged` holds the merged Result
    RangeDone, ///< this --slice-range is done, other slices are elsewhere
    Paused,    ///< stopped on request (Ctrl-C / DIR/PAUSE); resumable
    Error,     ///< refused or failed; message in `err`
};

/// Pause request flag: 0 none, 1 graceful (stop after the current slice).
/// Set by the CLI's SIGINT handler; DIR/PAUSE is polled between slices too.
[[nodiscard]] std::atomic<int>& pause_flag() noexcept;

/// Drives a checkpointed run (see the file comment), with progress on stderr.
/// On Complete it also writes DIR/merged.json and fills `info` if non-null.
/// Error for a non-empty DIR without run.json (unless `force`), a DIR/PAUSE
/// that cannot be removed, and a slice path that read_slice_file refuses unread.
[[nodiscard]] Outcome run(RunConfig cfg,
                          const SliceRunner& runner,
                          verify::Result& merged,
                          std::string& err,
                          MergeInfo* info = nullptr);

/// Loads every slice_*_of_*.json in each dir, for `collatz merge`. A path that
/// is not a directory or an unparseable file is reported in `err` and fails the
/// load.
[[nodiscard]] bool
load_slices(const std::vector<std::string>& paths, std::vector<Slice>& out, std::string& err);

} // namespace collatz::checkpoint

#endif // COLLATZ_CHECKPOINT_HPP
