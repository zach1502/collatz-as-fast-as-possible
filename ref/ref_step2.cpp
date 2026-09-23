// Reference Step-2 look-ahead sieve. See ref/include/collatz_ref/ref_step2.hpp
// for the specification. It shares no code with src/core/step2.cpp.

#include "collatz_ref/ref_step2.hpp"

#include "collatz/pow3.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace collatz::ref {
namespace {

/// One accelerated step on a residue iterate; u64 is exact because dip_B
/// iterates stay below 3^32 < 2^51.
[[nodiscard]] std::uint64_t step_T_u64(std::uint64_t x) noexcept {
    return (x & 1u) ? (3 * x + 1) / 2 : x / 2;
}

/// The margin term A_k(m) = 306k - 485 f_k, signed because it is negative once
/// 485 f_k > 306k.
[[nodiscard]] long margin(unsigned k, unsigned fk) noexcept {
    return 306L * static_cast<long>(k) - 485L * static_cast<long>(fk);
}

/// Window chain-merge gain: explores the backward preimage tree from the
/// concrete window iterate `value` to depth `depth_left`, recording the best
/// contraction gain (485 v - 306 L) over admissible value-decreasing chains
/// with odd-step count 1 <= v <= o_max (ratio 2^L/3^v < 1). Backward steps:
/// even preimage u = 2w (T(u)=w, always integral) and odd preimage u=(2w-1)/3
/// when w == 2 (mod 3) (u odd, T(u)=w). The class of `value` mod 3^{t_w} fixes
/// every mod-3 branch test up to v <= t_w, so the result equals
/// preimage::WindowChainTable::gain(value mod 3^{t_w}, o_max). Returns
/// the best gain found, or kNoGain when no decreasing chain of that class
/// exists. Values L, v <= 2*t_w <= 20, so 2^L / 3^v fit in u128.
constexpr long kNoGain = -1000000L;

long window_chain_gain(std::uint64_t value,
                       unsigned L,
                       unsigned v,
                       unsigned o_max,
                       unsigned t_w,
                       unsigned depth_left,
                       long best) {
    if (v >= 1 && v <= o_max) {
        // ratio 2^L / 3^v < 1  <=>  2^L < 3^v.
        if ((static_cast<collatz::u128>(1) << L) < collatz::pow3(v)) {
            const long g = 485L * static_cast<long>(v) - 306L * static_cast<long>(L);
            if (g > best) {
                best = g;
            }
        }
    }
    if (depth_left == 0) {
        return best;
    }
    // Even preimage (always integral): value grows, ratio *2.
    best = window_chain_gain(2u * value, L + 1, v, o_max, t_w, depth_left - 1, best);
    // Odd preimage: only when value == 2 (mod 3) and v < t_w, so that the
    // residue mod 3^{t_w} still decides admissibility.
    if (v < t_w && (value % 3u) == 2u) {
        best = window_chain_gain(
            (2u * value - 1u) / 3u, L + 1, v + 1, o_max, t_w, depth_left - 1, best);
    }
    return best;
}

} // namespace

bool parity_mod3_is_2(const std::vector<unsigned char>& par, unsigned k) {
    bool had_odd = false;
    unsigned last_odd = 0;
    for (unsigned s = 0; s < k; ++s) {
        if (par[s] != 0) {
            had_odd = true;
            last_odd = s;
        }
    }
    if (!had_odd) {
        return false; // arithmetic mod-3 not determined by parities.
    }
    // Even number of even steps since the last odd step: steps last_odd+1 .. k-1
    // are all even (last_odd is the last odd step), so their count is (k-1)-last_odd.
    const unsigned even_since = (k - 1) - last_odd;
    return (even_since % 2) == 0;
}

std::uint32_t dip_B(std::uint64_t m, unsigned B, unsigned window_chains, unsigned window_templates_D) {
    const std::uint64_t bmask = (B >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << B) - 1);
    std::uint64_t v = m & bmask;

    // Parity sequence par[0..B-1], prefix odd-step counts f_k for k in [0,B], and
    // the iterate values val[k] = T^k(m) (< 3^B < 2^51, exact in u64) for the
    // window chain-merge class lookup.
    std::vector<unsigned char> par(B);
    std::vector<unsigned> f(static_cast<std::size_t>(B) + 1, 0);
    std::vector<std::uint64_t> val(static_cast<std::size_t>(B) + 1);
    val[0] = v;
    for (unsigned s = 0; s < B; ++s) {
        par[s] = static_cast<unsigned char>(v & 1u);
        f[s + 1] = f[s] + par[s];
        v = step_T_u64(v);
        val[s + 1] = v;
    }

    long best = 0; // A_0 = 0 is always in S_B(m).

    // (A) descent margins A_k, for 0 <= k <= B.
    for (unsigned k = 0; k <= B; ++k) {
        best = std::max(best, margin(k, f[k]));
    }
    // (B) path-merge margins A_k + 179, when T^k(m) == 2 (mod 3) by the parity
    //     criterion (requires >= 1 odd step). Depth k in [1, B].
    for (unsigned k = 1; k <= B; ++k) {
        if (parity_mod3_is_2(par, k)) {
            best = std::max(best, margin(k, f[k]) + 179L);
        }
    }
    // (C) OEE margins A_k + 306, when step k begins l >= 1 odd steps then 2 even
    //     steps, all inside the B-step window (indices < B).
    for (unsigned k = 0; k < B; ++k) {
        if (par[k] == 0) {
            continue; // must begin on an odd step.
        }
        unsigned e = k;
        while (e < B && par[e] != 0) {
            ++e; // first even step at or after k (l = e - k >= 1).
        }
        if (e >= B || (e + 1u) >= B || par[e + 1u] != 0) {
            continue; // need two even steps within the window.
        }
        best = std::max(best, margin(k, f[k]) + 306L);
    }
    // (D) window chain-merge margins A_k + (485 v - 306 L): the best admissible
    //     decreasing backward preimage chain of val[k]'s class mod 3^{t_w}, with
    //     odd-step count v <= min(f_k, t_w) so word-determinism (f_k >= v) makes
    //     the window walk's class equal the real iterate's class mod 3^v. Only
    //     applied when window_chains (= t_w) > 0; t_w = 0 leaves best untouched.
    if (window_chains > 0) {
        for (unsigned k = 0; k <= B; ++k) {
            if (f[k] < 1) {
                continue; // need >= 1 odd step (word-determinism cap v <= f_k).
            }
            const unsigned o_max = std::min<unsigned>(f[k], window_chains);
            const long g =
                window_chain_gain(val[k], 0, 0, o_max, window_chains, 2u * window_chains, kNoGain);
            if (g > 0) {
                best = std::max(best, margin(k, f[k]) + g);
            }
        }
    }
    // (E) general merge-template margins. Every general (u, w) template's dip
    //     term equals an empty-w ancestor chain of odd depth o_u <= D applied at
    //     a window step (word-determinism gate o_u <= f_k), i.e. the same
    //     backward-preimage walk carried to the deeper BFS depth D with odd-step
    //     cap min(f_k, min(D, max_window_t=12)). This walk is the counterpart of
    //     the deeper WindowChainTable (t = min(D, 12), depth = D). The max over
    //     both walks keeps dip monotone in D. D = 0 leaves best untouched.
    if (window_templates_D > 0) {
        const unsigned t_tmpl = std::min<unsigned>(window_templates_D, 12u); // max_window_t.
        for (unsigned k = 0; k <= B; ++k) {
            if (f[k] < 1) {
                continue; // need >= 1 odd step (word-determinism cap v <= f_k).
            }
            const unsigned o_max = std::min<unsigned>(f[k], t_tmpl);
            const long g =
                window_chain_gain(val[k], 0, 0, o_max, t_tmpl, window_templates_D, kNoGain);
            if (g > 0) {
                best = std::max(best, margin(k, f[k]) + g);
            }
        }
    }

    return static_cast<std::uint32_t>(best); // best >= 0 by A_0.
}

unsigned fmin_for(unsigned N, unsigned A) {
    const unsigned M = N - A;
    // ceil((306 M + 1) / 485).
    const unsigned long num = 306UL * M + 1UL;
    return static_cast<unsigned>((num + 484UL) / 485UL);
}

bool must_check(collatz::u128 n0,
                collatz::u128 m,
                unsigned f,
                unsigned a,
                unsigned N,
                unsigned A,
                unsigned B,
                unsigned num_bitvectors,
                bool partial_oee,
                unsigned a_star,
                unsigned window_chains,
                unsigned window_templates_D) {
    const unsigned M = N - A;
    const collatz::u128 n = n0 + (static_cast<collatz::u128>(a) << M);

    // (1) mod-9 preimage sieve: prune n == 2,4,5,8 (mod 9).
    const unsigned r9 = static_cast<unsigned>(n % 9);
    if (r9 == 2 || r9 == 4 || r9 == 5 || r9 == 8) {
        return false;
    }

    // (2) look-ahead bitvector. threshold(f) = 485 f - 306 M. Prune iff
    //     dip_B(m_a mod 2^B) >= threshold, for f < fmin + num_bitvectors (the
    //     f < fmin case has threshold <= 0 and is pruned automatically).
    if (f < fmin_for(N, A) + num_bitvectors) {
        const long threshold = 485L * static_cast<long>(f) - 306L * static_cast<long>(M);
        const collatz::u128 m_a = m + collatz::pow3(f) * static_cast<collatz::u128>(a);
        const std::uint64_t bmask = (B >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << B) - 1);
        const auto residue = static_cast<std::uint64_t>(m_a & static_cast<collatz::u128>(bmask));
        if (static_cast<long>(dip_B(residue, B, window_chains, window_templates_D)) >= threshold) {
            return false;
        }
    }

    // (3) high-bit partial OEE: prune a >= a_star.
    if (partial_oee && a >= a_star) {
        return false;
    }

    return true;
}

std::vector<Candidate> candidates(unsigned N,
                                  unsigned A,
                                  unsigned B,
                                  unsigned num_bitvectors,
                                  bool partial_oee,
                                  const Options& step1_opts,
                                  unsigned window_chains,
                                  unsigned window_templates_D) {
    const unsigned M = N - A;
    const auto surv = ref::survivors(M, step1_opts);
    const std::uint32_t twoA = 1u << A;

    // Partial-OEE threshold options: the OEE arm only, as in the verifier's mask.
    Options oee_only;
    oee_only.descent = false;
    oee_only.path_merge = false;
    oee_only.oee = true;
    oee_only.mod3t = 0;

    std::vector<Candidate> out;
    for (const auto& s : surv) {
        const collatz::u128 n0 = s.n0;
        const collatz::u128 m = s.m;
        const unsigned f = s.f;

        unsigned a_star = twoA; // "no prune" sentinel unless partial OEE fires.
        if (partial_oee) {
            const unsigned raw = ref::partial_oee_a_star(n0, M, A, oee_only);
            // partial_oee_a_star returns UINT32_MAX when nothing is excluded.
            a_star = (raw >= twoA) ? twoA : raw;
        }

        for (std::uint32_t a = 0; a < twoA; ++a) {
            if (must_check(n0,
                           m,
                           f,
                           a,
                           N,
                           A,
                           B,
                           num_bitvectors,
                           partial_oee,
                           a_star,
                           window_chains,
                           window_templates_D)) {
                Candidate c;
                c.n = n0 + (static_cast<collatz::u128>(a) << M);
                c.m_a = m + collatz::pow3(f) * static_cast<collatz::u128>(a);
                c.f = f;
                out.push_back(c);
            }
        }
    }

    std::sort(
        out.begin(), out.end(), [](const Candidate& x, const Candidate& y) { return x.n < y.n; });
    return out;
}

} // namespace collatz::ref
