#ifndef COLLATZ_VERIFY_HPP
#define COLLATZ_VERIFY_HPP

// End-to-end Collatz verifier: proves that every n < 2^N drops below itself or
// is covered by a smaller number, from Step 1 (residue-class sieve) to Step 3
// (trajectory finisher).
//
// A preflight walks every n < 2^17 exactly with the Barina walk. It is the
// induction base and covers the n >= 99781 precondition of the descent theorems
// Step 1 uses. Step 1 enumerates the surviving residue classes mod 2^M
// (M = N - A). When A > 0, Step 2 builds a look-ahead filter once and decides,
// for each Step-1 survivor, which of the 2^A high-bit extensions
// n = n0 + a*2^M (a in [0, 2^A)) must still be checked; when A == 0 each
// survivor n0 is its own single candidate. Step 3 finishes the trajectory of
// each remaining candidate n, from m = T^M(n) = node.m + 3^f*a, until it drops
// below n. Candidates and preflight numbers fold into two checksums that do not
// depend on the thread count.
//
// Block mode (Options::block_h = h > 0) checks [h*2^N, (h+1)*2^N) instead,
// given that every smaller number converges: no preflight, and each Step-1
// survivor is lifted into the block before Step 2 (frontier.hpp).

#include "collatz/u128.hpp"
#include "collatz/verify/bounds.hpp"
#include "collatz/verify/defaults.hpp"
#include "collatz/verify/options.hpp"
#include "collatz/verify/records.hpp"
#include "collatz/verify/result.hpp"

#include <cstdint>

namespace collatz::verify {

/// Runs the verifier. When A > 0 the Step 2 look-ahead is built and applied.
/// Throws std::invalid_argument if the Step 2 parameters are out of range
/// (A > step2::max_A, B outside [2, step2::max_B], num_bitvectors == 0, or
/// A >= N).
[[nodiscard]] Result run(const Options& opts);

/// x mod (2^61 - 1), exact for every x < 2^128. The coverage/pow folds sum their
/// u64 terms raw in u128 and reduce once per thread; <= 2^64 terms stay < 2^128.
[[nodiscard]] std::uint64_t mod_mersenne61_u128(u128 x) noexcept;

} // namespace collatz::verify

#endif // COLLATZ_VERIFY_HPP
