#ifndef COLLATZ_LINEAR_HPP
#define COLLATZ_LINEAR_HPP

// Linear Collatz verifier in the style of Barina.
//
// Walks every starting value n0 in a half-open interval [lo, hi) until it drops
// below n0, which by induction over the interval shows that every n < hi
// converges when every n < lo does. Instead of the plain map it uses David
// Barina's count-trailing-zeros acceleration: an odd run is stripped and
// multiplied by a power of three from the compile-time POW3 table, many T-steps
// per iteration. See the notes in verify_one_detailed (linear.cpp).
//
// Correctness aids:
//   * every 3^a multiply is overflow-checked; on u128 overflow the n0 is
//     recomputed end to end by an arbitrary-precision GMP "oracle" copy of the
//     same loop (counted in Result::overflow_fallbacks);
//   * two independent sieves prune starting values already implied by smaller
//     ones (a 2^k descent bitmap and a mod-9 residue filter);
//   * the range splits into fixed chunks farmed to a thread pool, and the
//     reduction (a wrapping checksum sum + a max) is order-independent, so the
//     result is identical for any thread count.

#include "collatz/u128.hpp"

#include <cstdint>
#include <gmpxx.h>
#include <vector>

namespace collatz::linear {

/// Options of a range verification.
struct Options {
    /// Descent-sieve modulus exponent k: residues mod 2^k proven to descend are
    /// skipped. 0 disables the 2^k sieve. Valid range [0, 30].
    unsigned sieve_bits = 24;
    /// The mod-9 image sieve (skips n congruent to 2, 4, 5, 8 mod 9).
    bool mod9 = true;
    /// Worker-thread count. 0 means "use hardware_threads()". 1 is serial.
    unsigned threads = 1;
};

/// Outcome of verifying a whole interval.
struct Result {
    /// Wrapping (mod 2^64) sum of all alpha exponents over all processed n0 --
    /// Barina's "proof of work" checksum. Thread-count independent.
    std::uint64_t checksum = 0;
    /// Largest value observed on any processed trajectory (the max excursion).
    u128 max_excursion = 0;
    /// The starting value n0 that produced max_excursion (smallest on ties).
    u128 max_excursion_n0 = 0;
    /// Count of n0 walked (not pruned by a sieve).
    std::uint64_t numbers_processed = 0;
    /// Count of n0 skipped by a sieve.
    std::uint64_t numbers_skipped_by_sieve = 0;
    /// Count of n0 that overflowed u128 and were recomputed via the GMP oracle.
    std::uint64_t overflow_fallbacks = 0;
    /// Wall-clock seconds spent in the verification loop (excludes sieve build).
    double seconds = 0.0;
};

/// The 2^k "descent" sieve. A residue class r mod 2^k is skipped when every
/// n congruent to r reaches a value below itself within k accelerated (T)
/// steps; such n0 need not be checked because they descend into the
/// already-verified region. Bit r of the bitmap is set iff class r is skipped.
class Sieve2k {
public:
    /// Builds the sieve for modulus 2^k, k in [0, 30], in O(2^k) time. k == 0
    /// disables it (skip returns false for everything).
    void build(unsigned k);

    /// True iff n's residue mod 2^k is provably descending (and thus skippable).
    [[nodiscard]] bool skip(u128 n) const noexcept;

    /// k from build(k): the sieve classifies residues mod 2^k (0 means off).
    [[nodiscard]] unsigned bits() const noexcept {
        return k_;
    }

private:
    unsigned k_ = 0;
    std::vector<std::uint64_t> bitmap_; ///< bit r set => class r skipped
};

/// True iff n is congruent to 2, 4, 5, or 8 modulo 9. Such n are images of
/// smaller integers under the Collatz map and so are already covered.
[[nodiscard]] bool mod9_skip(u128 n) noexcept;

/// Detailed single-n0 result, exposed for testing the kernel directly.
struct OneResult {
    std::uint64_t checksum = 0; ///< sum of alphas for this n0
    u128 max_seen = 0;          ///< max excursion until first drop below n0
    bool overflowed = false;    ///< true iff the GMP oracle fallback was used
    /// When `overflowed` and the true max exceeds 2^128-1, `max_seen` is
    /// saturated to u128_max (the checksum is always exact).
};

/// Verifies one starting value with the u128 kernel, falling back to the GMP
/// oracle on overflow, and reports whether the fallback ran.
[[nodiscard]] OneResult verify_one_detailed(u128 n0);

/// Arbitrary-precision GMP reference: the same Barina loop over mpz, so it
/// never overflows. Returns the checksum and fills `max_seen` (exact, may
/// exceed u128). The u128 kernel falls back to it on overflow, and the tests
/// compare against it.
std::uint64_t verify_one_oracle(u128 n0, mpz_class& max_seen);

/// Verifies every starting value in [lo, hi) with `opts`.
[[nodiscard]] Result verify_range(u128 lo, u128 hi, const Options& opts);

} // namespace collatz::linear

#endif // COLLATZ_LINEAR_HPP
