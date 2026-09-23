// Step 2 of Angeltveit's sieve: the per-survivor mask lookup (Filter::mask, the
// window reads it inlines, and the burst window touch touch2). The mathematics is
// in collatz/step2.hpp.
//
// step2_filter.cpp has the constructor, step2_dip.cpp the dip walks, and
// step2_build.cpp, step2_pack.cpp and step2_coldbv*.cpp the bitvector build.

#include "collatz/step2.hpp"

#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "step2_internal.hpp"

#include <cstdint>
#include <span>

namespace collatz::step2 {

using namespace detail;

unsigned Filter::mod9(u128 n) noexcept {
    // 2^64 == 7 (mod 9): fold each 64-bit limb, weight the high limb by 7.
    const std::uint64_t lo = lo64(n) % 9u;
    const std::uint64_t hi = hi64(n) % 9u;
    return static_cast<unsigned>((lo + 7u * hi) % 9u);
}

namespace {

/// Reads a `width`-bit (<= 64) contiguous window from a packed 2^B-bit vector
/// starting at bit `j0`, wrapping around at the 2^B boundary. `span` = 2^B.
[[nodiscard]] std::uint64_t read_window(std::span<const std::uint64_t> bv,
                                        std::uint64_t j0,
                                        unsigned width,
                                        std::uint64_t span) noexcept {
    const auto extract = [&bv](std::uint64_t start, unsigned w) -> std::uint64_t {
        if (w == 0) {
            return 0;
        }
        const std::uint64_t word = start >> 6;
        const unsigned off = static_cast<unsigned>(start & 63u);
        std::uint64_t bits = bv[word] >> off;
        if (off + w > 64u) {
            bits |= bv[word + 1] << (64u - off);
        }
        const std::uint64_t m = (w >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << w) - 1);
        return bits & m;
    };

    if (j0 + width <= span) {
        return extract(j0, width);
    }
    // Wraps the 2^B boundary: split into [j0, span) and [0, remainder).
    const unsigned first = static_cast<unsigned>(span - j0);
    const std::uint64_t lo = extract(j0, first);
    const std::uint64_t hi = extract(0, width - first);
    return lo | (hi << first);
}

/// Reads a 2^A-bit (`twoA`, <= 16384) window from a packed 2^B-bit vector
/// starting at bit `j0` (< span), wrapping at the 2^B boundary, into the low
/// `words` words of `out` (all written; no prior zeroing needed), each word
/// ANDed with `and_mask[i]`.
void read_window_wide(CandidateMask& out,
                      std::span<const std::uint64_t> bv,
                      std::uint64_t j0,
                      std::uint32_t twoA,
                      unsigned words,
                      std::uint64_t span,
                      const std::uint64_t* and_mask) noexcept {
    out.words = words;
    if (span < 64u) {
        // B < 6: sub-word vector, read word by word with read_window; the
        // packer's replicated pattern makes each bit equal bit (j0 + a) mod 2^B.
        const std::uint64_t span_mask = span - 1;
        for (unsigned wi = 0; wi < words; ++wi) {
            const std::uint64_t start = (j0 + static_cast<std::uint64_t>(wi) * 64u) & span_mask;
            const std::uint32_t remaining = twoA - wi * 64u;
            const unsigned width = remaining >= 64u ? 64u : remaining;
            out.w[wi] = read_window(bv, start, width, span) & and_mask[wi];
        }
        return;
    }
    const std::uint64_t word_mask = (span >> 6) - 1;
    const std::uint64_t idx = j0 >> 6;
    const unsigned shift = static_cast<unsigned>(j0 & 63u);
    if (shift == 0) {
        for (unsigned i = 0; i < words; ++i) {
            out.w[i] = bv[(idx + i) & word_mask] & and_mask[i];
        }
    } else {
        for (unsigned i = 0; i < words; ++i) {
            const std::uint64_t q = (idx + i) & word_mask;
            out.w[i] =
                ((bv[q] >> shift) | (bv[(q + 1) & word_mask] << (64u - shift))) & and_mask[i];
        }
    }
    if (twoA < 64u) {
        out.w[0] &= (std::uint64_t{1} << twoA) - 1; // A < 6: one partial word.
    }
}

} // namespace

CandidateMask Filter::mask(const step1::Node& survivor) const {
    // f < fmin: 485 f <= 306 (N-A), so dip >= 0 >= threshold for every a.
    CandidateMask bv_window;
    bv_window.words = words_;
    if (survivor.f < fmin_) {
        bv_window.zero();
        return bv_window;
    }

    const unsigned i = survivor.f - fmin_;
    // mod-9 sieve, fused into the window read.
    const std::uint64_t* m9 = m9_[mod9(survivor.n0)][pow2m_mod9_].w.data();
    if (i >= params_.num_bitvectors) {
        // Beyond the last bitvector: no lookahead pruning, only the mod-9 mask.
        for (unsigned wi = 0; wi < words_; ++wi) {
            bv_window.w[wi] = m9[wi];
        }
    } else {
        const std::uint64_t span = std::uint64_t{1} << params_.B;
        const std::uint64_t m_res = lo64(survivor.m) & b_mask_; // m mod 2^B.
        const std::uint64_t j0 = (m_res * inv3f_[i]) & b_mask_;
        read_window_wide(bv_window, bv_[i], j0, twoA_, words_, span, m9);
    }

    // High-bit partial OEE pruning: extensions a >= a* are excluded (a < a* kept).
    if (partial_oee_) {
        // The OEE a* is carried down the Step-1 DFS (step1::Node::oee_astar,
        // enabled via step1::Options::track_oee_astar), so there is no
        // per-survivor O(M) prefix walk. The no-prune sentinel kOeeAStarNone
        // compares >= 2^A.
        const u128 a_star = survivor.oee_astar;
        if (a_star < static_cast<u128>(twoA_)) {
            bv_window.and_with(prefix_mask(static_cast<unsigned>(a_star), words_));
        }
    }

    return bv_window;
}

std::uint64_t Filter::touch2(const step1::Node& survivor) const noexcept {
    // Unsigned wrap: f < fmin gives i >= num_bitvectors, i.e. no window read in
    // mask() either. B < 6: the whole bitvector is one word (already hot).
    const unsigned i = survivor.f - fmin_;
    if (i >= params_.num_bitvectors || params_.B < 6u) {
        return 0;
    }
    const std::uint64_t word_mask = (std::uint64_t{1} << (params_.B - 6u)) - 1;
    const std::uint64_t m_res = lo64(survivor.m) & b_mask_;
    const std::uint64_t j0 = (m_res * inv3f_[i]) & b_mask_;
    const std::uint64_t idx = j0 >> 6;
    const std::uint64_t* const w = bv_[i].data();
    // When j0 is word-aligned mask() reads only idx .. idx + words_ - 1, so the
    // last touch may fetch one unused line: harmless.
    const std::uint64_t last = (idx + words_) & word_mask;
    std::uint64_t sink = w[idx] + w[last];
    if (words_ > 8u) {
        sink += w[(idx + 8u) & word_mask];
    }
    return sink;
}

} // namespace collatz::step2
