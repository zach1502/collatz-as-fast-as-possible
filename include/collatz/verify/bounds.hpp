#ifndef COLLATZ_VERIFY_BOUNDS_HPP
#define COLLATZ_VERIFY_BOUNDS_HPP

// The un-walked peak bound, the value range of a block, block validation,
// the exact-max tests, and the thread and bitvector limits of the command line.

#include "collatz/u128.hpp"
#include "collatz/verify/options.hpp"
#include "collatz/verify/result.hpp"

#include <string>

namespace collatz::verify {

/// The deepest trajectory step at which any enabled exclusion certificate
/// can fire, so every never-walked value of an excluded x < 2^N is T^j(x) for
/// some j <= this depth. K = M + (A > 0 ? B : 0), with M = N - A.
[[nodiscard]] unsigned unwalked_depth(const Options& opts) noexcept;

/// Base-2 logarithm of the un-walked peak bound
/// `(block_h + 1) * 2^N * (3/2)^depth`, for human-readable reporting
/// (`= N + depth * (log2 3 - 1) + log2(block_h + 1)`). A full run passes
/// block_h = 0.
[[nodiscard]] double unwalked_bound_log2(unsigned N, unsigned depth, u128 block_h) noexcept;

/// Bit length of the largest number a run checks, (block_h + 1) * 2^N - 1: the
/// value width, as opposed to the Step-1 depth N. Equals N for a full run.
[[nodiscard]] unsigned value_bits(const Options& opts) noexcept;

/// First number of the run's range, block_h * 2^N (0 for a full run). Valid
/// when value_bits(opts) <= 128 (validate_block guarantees it in block mode).
[[nodiscard]] u128 block_start(const Options& opts) noexcept;

/// Throws std::invalid_argument when block mode (block_h > 0) cannot run these
/// options: A == 0, M = N - A below 2 (Step 1 starts at depth 2 and would
/// enumerate no class of the block), block start below 2^17 (the floor of
/// Theorem 4.1), or block end above 2^96 or too large for the u128 lift
/// ((block end)*(3/2)^(N-A) >= 2^125); and, for any run, a malformed
/// record_threshold. A no-op for a full run apart from that check. run() calls
/// it.
void validate_block(const Options& opts);

/// The largest explicit thread count that `verify`, `certify` and `worker`
/// accept for --threads: 4 per hardware thread, and at least 256. A result does
/// not depend on the count, and the run sizes per-thread buffers and spawns
/// threads by it, so a larger value only costs memory or fails. 0, which means
/// every hardware thread, is always accepted.
[[nodiscard]] unsigned max_threads() noexcept;

/// The largest --bitvectors value the command line and a work-server lease
/// accept. Each Step-2 bitvector holds 2^B bits (512 MiB at B = 32).
inline constexpr unsigned kMaxBitvectors = 32;

/// Decimal of the largest excursion in `r`: max_excursion_wide when set, else
/// max_excursion.
[[nodiscard]] std::string max_excursion_decimal(const Result& r);

/// log2 of the largest excursion in `r` (wide values included), 0 when there is
/// none. For display.
[[nodiscard]] double max_excursion_log2(const Result& r);

/// Exact-max test for a full run (block_h == 0) or a block, against the
/// record in `r` (wide when max_excursion_wide is set): true iff
/// (block_h + 1) * 2^N * 3^depth < 2^depth * R in exact integers (GMP). For a
/// block this says every never-walked value of an x in [0, (block_h + 1) * 2^N)
/// is below R. A strict test, so no un-walked value can even equal the record:
/// the argmax is exact too. Proof: 2^k * T^k(v) < (v + 1) * 3^k for every v and
/// k, so every un-walked value T^j(x), x < X = (block_h + 1) * 2^N,
/// j <= depth, is below X * (3/2)^depth.
/// True for a full run with N <= 17 (the preflight walks everything). False
/// when max_exceeds_u128 is set without the exact wide value, or R == 0.
[[nodiscard]] bool result_max_exact(unsigned N, u128 block_h, unsigned depth, const Result& r);

} // namespace collatz::verify

#endif // COLLATZ_VERIFY_BOUNDS_HPP
