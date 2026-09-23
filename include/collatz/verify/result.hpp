#ifndef COLLATZ_VERIFY_RESULT_HPP
#define COLLATZ_VERIFY_RESULT_HPP

// verify::Result: the folds, digests, records and telemetry of a run.

#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace collatz::verify {

/// One entry of Result::records.
struct RecordHit {
    u128 n = 0;       ///< Walked start value.
    std::string peak; ///< Its exact excursion, decimal.
};

/// Aggregate result of a verification run. Every scalar is independent of the
/// thread count.
struct Result {
    std::uint64_t candidates_checked = 0; ///< Preflight numbers + checked (n0,a) candidates.
    std::uint64_t step2_masked_out = 0;   ///< Extensions a pruned by Step 2 (0 when A == 0).
    std::uint64_t step3_steps_total = 0;  ///< 16-step blocks walked in Step 3 (= blocks_total).
    std::uint64_t coverage_checksum = 0;  ///< Set-fold over checked n (mod 2^61-1).
    /// Result digest, version 5: a digest over the checked candidates that does
    /// not depend on threads, batches, lanes or backend. For each candidate n
    /// the 16-step block recurrence from m yields `blocks` (number of blocks
    /// executed) and `v_end` (the landing value after the last block: the first
    /// block-boundary value < n; n itself for the n <= 2 base case, blocks ==
    /// 0). The per-candidate term is
    ///   key = lo64(n) ^ rotl64(hi64(n),29) ^ (blocks * 0xBF58476D1CE4E5B9)
    ///         ^ (lo64(v_end) * 0x94D049BB133111EB) ^ rotl64(hi64(v_end),43)
    ///   d   = mix64(key)
    /// with mix64 the splitmix64 finalizer, and the digest is Sum d over
    /// candidates mod 2^61-1 (accumulated raw per thread/batch, reduced once at
    /// merge). Unlike a plain sum of block counts, any single wrong
    /// (n, blocks, v_end) triple changes it with probability ~1-2^-61, and so
    /// does a pair of compensating +1/-1 block errors. `pow_available` reports
    /// whether it is meaningful and `pow_kernel_id` tags the block width. The
    /// CPU table16 kernel and both GPU tail kernels print the identical digest
    /// (same recurrence).
    std::uint64_t pow_checksum = 0;
    /// Diagnostic: the wrapping u64 sum of the block counts over checked
    /// candidates; it equals `step3_steps_total`. Two compensating block errors
    /// cancel in this sum, which is why pow_checksum hashes each candidate.
    std::uint64_t blocks_total = 0;
    /// True iff pow_checksum is meaningful: the table16 block recurrence
    /// produced it.
    bool pow_available = false;
    /// The block width of the recurrence behind pow_checksum: 16 when
    /// pow_available is set, else 0. Block counts of different widths are not
    /// comparable, so pow equality is asserted only when this id matches.
    unsigned pow_kernel_id = 0;
    /// Largest excursion over checked candidates. A candidate's excursion is the
    /// largest T^j(n) before its orbit first drops below n
    /// (step3::first_drop_peak); max_excursion_wide and records use the same.
    u128 max_excursion = 0;
    u128 max_excursion_n = 0;      ///< Candidate attaining it (smallest on ties).
    bool max_exceeds_u128 = false; ///< True iff some excursion exceeded 2^128-1.
    /// Exact decimal value of the largest excursion when it exceeds 2^128-1
    /// (max_exceeds_u128; max_excursion then holds u128_max and
    /// max_excursion_n the exact argmax). Empty otherwise. Both the CPU and GPU
    /// paths fill it from the GMP replay of the excursion walk.
    std::string max_excursion_wide;
    /// Walked candidates whose excursion exceeds Options::record_threshold,
    /// ascending in n, each with its exact excursion (see max_excursion). Empty
    /// when the threshold is off. Holds at most Options::records_cap entries:
    /// the smallest n (see records_truncated).
    std::vector<RecordHit> records;
    /// True iff more than Options::records_cap walked candidates exceeded the
    /// threshold, so `records` holds only the records_cap smallest of them.
    /// Thread-count independent (the kept entries are the smallest n, not the
    /// first found).
    bool records_truncated = false;
    /// Exact-max soundness flag. True iff the
    /// reported max_excursion is provably the exact global maximum: the record
    /// strictly exceeds the un-walked-trajectory peak bound
    /// (2^N * 3^K < 2^K * max_excursion, K = unwalked_depth(opts) = M + B), so no
    /// value on a never-walked (Step-1 class- / Step-2 window-excluded)
    /// trajectory prefix T^j(x), j <= K, can equal or beat it. Also true
    /// unconditionally for N <= kPreflightBits (the preflight walks every
    /// n < 2^N directly). When false the max_excursion is only a lower bound by
    /// this argument alone; tests/integration/test_verify_oracle.cpp checks it
    /// at N=30 against the independent linear verifier. Set by
    /// result_max_exact().
    ///
    /// Block mode (block_h > 0) uses the block end instead of 2^N:
    /// (block_h + 1) * 2^N * 3^K < 2^K * max_excursion. Then the flag bounds the
    /// never-walked prefixes within the block: no value T^j(x), j <= K, of an
    /// excluded block member x reaches the record. It does not make
    /// max_excursion the block's maximum, because an excluded member can climb
    /// on the orbit of a target below the block start after its certificate
    /// step; the maximum below the block end is max(G(L), max_excursion), with
    /// G(L) the maximum below the block start. No preflight shortcut applies.
    bool max_exact = false;
    /// Candidates whose Step-3 block walk the GMP oracle finished because a
    /// value left u128. A tail counts only when its walk passed 2^128-1 (or its
    /// m_a did); a replay that stayed below 2^128 depends on the running record
    /// bar and is not counted, nor are the excursion walks of record
    /// contenders. Counted so, the value does not depend on the schedule, and a
    /// block gives the same value on the CPU and GPU paths
    /// (test_integration_gpu_frontier_blocks).
    std::uint64_t overflow_fallbacks = 0;
    /// Diagnostic: candidates whose exact prefix max had to be re-walked because
    /// their upper bound reached the running record. It counts record
    /// contenders, so it depends on the order candidates are visited and on the
    /// thread partition; it is not a thread-count-independent result (the
    /// checksums, counts and max excursion are). Zero for small N.
    std::uint64_t prefix_walks = 0;
    step1::Stats step1{};         ///< Step 1 enumeration statistics.
    double seconds_step1_2 = 0.0; ///< Step-2 filter build plus step1.seconds.
    double seconds_total = 0.0;   ///< Wall-clock for the whole run.
    /// Step-2 filter telemetry. `seconds_filter` is the wall time to build or
    /// load the look-ahead filter (part of seconds_step1_2). `bv_cache_hit` is
    /// true iff its packed bitvectors were loaded from the on-disk cache (warm
    /// run). 0/false for A == 0. Telemetry only; every other field is
    /// bit-identical hit or miss.
    double seconds_filter = 0.0;
    bool bv_cache_hit = false;
    /// The preflight (every n < 2^17) contribution to the mergeable scalars, for
    /// slice merges. The preflight is re-run identically in every case slice
    /// c/k (it is not itself sliced), so a k-way merge of slice JSONs must
    /// subtract this contribution (k-1) times to avoid counting it k times.
    /// `preflight_coverage`/`preflight_pow` are already reduced mod 2^61-1;
    /// `preflight_blocks`/`preflight_step3_steps` are the wrapping-u64
    /// partials; `preflight_candidates` is the exact count. For any N >= 17
    /// these are identical across every slice of a run (same kernel, same 2^17
    /// range), so checkpoint::merge cross-checks that before subtracting. Zero
    /// on the GPU backend, which does not populate them (the CPU merge path
    /// does), and in block mode (block_h > 0), which runs no preflight.
    std::uint64_t preflight_candidates = 0;
    std::uint64_t preflight_coverage = 0;
    std::uint64_t preflight_pow = 0;
    std::uint64_t preflight_blocks = 0;
    std::uint64_t preflight_step3_steps = 0;
    /// GPU per-batch self-check (--gpu only). `batches` counts checked
    /// device dispatches, `retried` the re-runs after a failed check,
    /// `cpu_fallback` the batches computed on the CPU after 3 failed re-runs, and
    /// `unchecked` the batches folded without the tail's self-check records.
    /// Diagnostic only; never changes a fold.
    bool gpu_selfcheck_on = false;
    std::uint64_t gpu_selfcheck_batches = 0;
    std::uint64_t gpu_selfcheck_retried = 0;
    std::uint64_t gpu_selfcheck_cpu_fallback = 0;
    std::uint64_t gpu_selfcheck_unchecked = 0;
    /// Host-pipeline counters (--gpu only; the stderr `gpu pipeline:` line), so
    /// tests can check that each path ran. Diagnostic only; never changes a
    /// fold and is not in the JSON / mergeable result set.
    struct GpuPipeline {
        std::uint64_t cpi_split_batches = 0; // id-buffer or flag-list overflows split in half
        std::uint64_t split_pipelined = 0;   // id-buffer splits sent through the pipeline
        std::uint64_t cand_cap_ids = 0;      // effective candidate-id cap (last batch)
        std::uint64_t budget_cap_ids = 0;    // memory-budget id cap (0 = not lowered)
    } gpu_pipeline;
    /// What the staged bitvector upload did (the stderr `gpu upload:` line).
    /// Diagnostic only, like gpu_pipeline.
    struct GpuUpload {
        bool early_upload = false;       // the staged bitvector upload ran
        std::uint64_t upload_chunks = 0; // staged chunks written
    } gpu_upload;
};

} // namespace collatz::verify

#endif // COLLATZ_VERIFY_RESULT_HPP
