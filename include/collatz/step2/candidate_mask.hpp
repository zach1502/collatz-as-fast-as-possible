#ifndef COLLATZ_STEP2_CANDIDATE_MASK_HPP
#define COLLATZ_STEP2_CANDIDATE_MASK_HPP

// The bitmask over a survivor's 2^A candidates (collatz/step2.hpp includes this
// file).

#include <array>
#include <cstdint>

namespace collatz::step2 {

/// Fixed-capacity bitmask over the 2^A candidate extensions of a Step-1
/// survivor (A <= max_A). Bit a (0 <= a < 2^A) is set iff n = n0 + a*2^{N-A}
/// must still be checked. A <= 6 uses one word (`words == 1`); A in {7,..,14}
/// uses 2/4/8/16/32/64/128/256 words. Only bits [0, 2^A) and the low `words`
/// words are meaningful; the array is sized for the widest A (256 words = 2 KB)
/// but not auto-zeroed: callers write or zero() exactly the `words` words they
/// use, so the A<=10 (16-word) and A<=6 (1-word) paths never touch the tail
/// words (a 2 KB struct that costs about its live prefix).
struct CandidateMask {
    /// 2^14 bits = 256 words (2 KB) is the widest supported (A == max_A == 14).
    static constexpr unsigned kMaxWords = 256;
    /// Not default-zeroed: only the low `words` words are ever read (every method
    /// loops `< words`), and every factory writes or zero()s exactly those. This
    /// keeps a per-survivor mask at its live prefix (128 B at A=10) rather than a
    /// 2 KB memset. Value-initialization (`CandidateMask{}`) still zeros all 256.
    std::array<std::uint64_t, kMaxWords> w;
    unsigned words = 1;

    /// Zeroes the low `words` words, for a factory that needs an all-clear base
    /// after setting `words` (e.g. the f<fmin "prune everything" mask, or a mask
    /// that is then OR-populated). Touches only the live prefix, not the 2 KB tail.
    void zero() noexcept {
        for (unsigned i = 0; i < words; ++i) {
            w[i] = 0;
        }
    }

    /// Number of set (must-check) bits across all words.
    [[nodiscard]] unsigned popcount() const noexcept {
        unsigned c = 0;
        for (unsigned i = 0; i < words; ++i) {
            c += static_cast<unsigned>(__builtin_popcountll(w[i]));
        }
        return c;
    }
    /// Candidate bit a; a must be < 2^A.
    [[nodiscard]] bool test(unsigned a) const noexcept {
        return ((w[a >> 6] >> (a & 63u)) & 1u) != 0u;
    }
    /// Word-wise AND (both masks must share `words`).
    void and_with(const CandidateMask& o) noexcept {
        for (unsigned i = 0; i < words; ++i) {
            w[i] &= o.w[i];
        }
    }
};

} // namespace collatz::step2

#endif // COLLATZ_STEP2_CANDIDATE_MASK_HPP
