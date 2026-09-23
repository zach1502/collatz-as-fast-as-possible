#ifndef COLLATZ_VERIFY_RECORDS_HPP
#define COLLATZ_VERIFY_RECORDS_HPP

// Record lists (Result::records), the block hypothesis recorded in the JSON
// and the default record threshold.

#include "collatz/u128.hpp"
#include "collatz/verify/options.hpp"
#include "collatz/verify/result.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace collatz::verify {

/// Sorts record hits ascending in n and keeps, for each n, the largest peak (a
/// candidate's prefix and tail can be offered separately).
[[nodiscard]] std::vector<RecordHit> merge_records(std::vector<RecordHit> hits);

/// Bounds one accumulator's record hits: `hits` holds (n, peak) pairs with
/// peaks below 2^128, `wide` the decimal peaks of the GMP replays. Keeps the
/// largest peak per n and, when more than `cap` distinct n remain, the `cap`
/// smallest; it then sets `limit` to the largest kept n, sets `dropped` and
/// erases the wide hits above `limit`. A dropped hit has `cap` smaller n in
/// this accumulator alone, so it is not among the run's `cap` smallest either:
/// the final list does not depend on how candidates were spread or on when a
/// prune ran. Shared by the CPU and GPU paths.
void prune_record_hits(std::vector<std::pair<u128, u128>>& hits,
                       std::vector<RecordHit>& wide,
                       std::size_t cap,
                       u128& limit,
                       bool& dropped);

/// The run's record list from its accumulators' hits: `raw` holds (n, peak)
/// pairs with peaks below 2^128, `wide` the decimal peaks of the GMP replays.
/// Returns the `cap` smallest distinct n, each with its largest peak,
/// ascending. `truncated` is set iff more than `cap` n qualified; `dropped`
/// says an accumulator already discarded hits because it held `cap` smaller n.
/// Shared by the CPU and GPU paths, so both build the list the same way.
[[nodiscard]] std::vector<RecordHit> finish_records(std::vector<std::pair<u128, u128>> raw,
                                                    std::vector<RecordHit> wide,
                                                    std::size_t cap,
                                                    bool dropped,
                                                    bool& truncated);

/// Default params.assumption_source for a block that starts at or below 2^71:
/// the published bound the hypothesis "every n < L converges" rests on.
inline constexpr const char* kAssumptionBarina71 =
    "Barina 2025, all n < 2^71 (doi 10.1007/s11227-025-07337-0)";
/// Default params.assumption_source for a block that starts above 2^71.
inline constexpr const char* kAssumptionPriorBlocks = "prior blocks of this project";
/// params.assumption_source of a full run (block_h == 0): the preflight is the
/// induction base, nothing is assumed.
inline constexpr const char* kAssumptionNone = "none (full run)";

/// The hypothesis source recorded for these options: Options::assumption_source
/// when set, else kAssumptionNone for a full run, kAssumptionBarina71 for a
/// block start L <= 2^71 and kAssumptionPriorBlocks for L > 2^71.
[[nodiscard]] std::string assumption_source(const Options& opts);

/// Decimal of the block start L = block_h * 2^N, below which every number is
/// assumed to converge ("0" for a full run). Exact for any block_h.
[[nodiscard]] std::string assumes_below(const Options& opts);

/// Barina's path record #97, n = kBarinaRecord97N < 2^71, and its peak in
/// T-map units (T(x) = (3x+1)/2 for odd x), as computed with GMP by walking n
/// until it drops below itself. Barina's path-records list gives the peak of
/// the 3n+1 map, twice this value. tests/core/test_verify_block.cpp recomputes it.
inline constexpr const char* kBarinaRecord97N = "2358909599867980429759";
inline constexpr const char* kBarinaRecord97PeakT = "494114680693632162318569565806744468898092";

/// The record threshold the CLI uses when --record-threshold is not given:
/// kBarinaRecord97PeakT for a block whose start L is at least 2^71, "" (off)
/// otherwise. Record #97 lies below L, so that peak is at most G(L), the
/// maximum below L; when it also exceeds the block's un-walked bound
/// (L + 2^N) * (3/2)^depth (it does for the frontier units, about 2^112 at
/// K = 50), the list holds every path record of the block.
[[nodiscard]] std::string default_record_threshold(const Options& opts);

} // namespace collatz::verify

#endif // COLLATZ_VERIFY_RECORDS_HPP
