// The GMP replay of the Step-2 window prunes: descent, path-merge and OEE
// targets within M or M + B steps, and backward chain merges from a window
// iterate; see certify.cpp.

#include "certify_internal.hpp"

#include <cstddef>
#include <gmpxx.h>
#include <vector>

namespace collatz::certify::detail {

/// True iff some descent / path-merge / OEE certificate with target strictly
/// below n fires within the first `maxk` steps of the prebuilt prefix. This is
/// the Theorem-4.4 direction: an excluded candidate must reduce to a smaller
/// already-verified integer. Each merge target y is checked explicitly: the
/// path-merge y = (2T^k(n)-1)/3 with T(y) == T^k(n), the OEE y = (m_j-1)/2 with
/// T^(e+1-j)(y) == T^(e+2)(n), both walked forward in GMP.
[[nodiscard]] bool reduces_below(const Prefix& p, const mpz_class& n, unsigned maxk) {
    for (unsigned k = 1; k <= maxk; ++k) {
        if (p.m[k] < n) {
            return true; // descent target.
        }
        if (mod3(p.m[k]) == 2) {
            const mpz_class y = (2 * p.m[k] - 1) / 3;
            if (y < n && step_T_mpz(y) == p.m[k]) {
                return true; // path-merge target.
            }
        }
    }
    // OEE: an odd arm m_j, l>=1 odd steps then 2 even steps, all within maxk,
    // whose one-below predecessor (m_j - 1)/2 is strictly below n.
    for (unsigned j = 0; j < maxk; ++j) {
        if (p.par[j] == 0) {
            continue;
        }
        unsigned e = j;
        while (e < maxk && p.par[e] != 0) {
            ++e; // first even step at or after j (l = e - j >= 1).
        }
        if (e >= maxk || (e + 1u) >= maxk || p.par[e + 1u] != 0) {
            continue; // need two even steps within [0, maxk).
        }
        const mpz_class y = (p.m[j] - 1) / 2;
        if (y < n && forward_meets(y, e + 1u - j, p.m[e + 2u])) {
            return true; // OEE merge target (Hercher: T^(e+2-j)(m_j) == T^(e+1-j)(y)).
        }
    }
    return false;
}

namespace {

/// Window-chain certificate in GMP: whether some backward preimage ancestor y
/// of `w` within `depth` backward T-steps has 1 <= y < n. The backward steps
/// are the even preimage u = 2w (T(u) = w, always integral) and, when
/// w == 2 (mod 3), the odd preimage u = (2w-1)/3 (u odd, T(u) = w). Such a y is
/// a smaller, already verified number whose orbit passes through w, so a
/// candidate n whose orbit reaches w = T^s(n) merges below itself. The search
/// goes to depth 2*t_w, at least as deep as the dip's window chains, so every
/// exclusion that the chain terms of the dip make has a witness here. It does
/// not use preimage::WindowChainTable. On success `y` holds the ancestor and
/// `L` its distance (T^L(y) == w); `dist` is the distance of `w` from the
/// search root. n3[d] holds n * 3^d for d <= depth.
[[nodiscard]] bool backward_merge_witness(const mpz_class& w,
                                          const mpz_class& n,
                                          const std::vector<mpz_class>& n3,
                                          unsigned depth,
                                          unsigned dist,
                                          mpz_class& y,
                                          unsigned& L) {
    // An odd preimage has u + 1 = 2(w + 1)/3 and an even one more, so every
    // ancestor within `depth` steps has 3^depth (u + 1) >= 2^depth (w + 1):
    // none is below n when 2^depth (w + 1) > 3^depth n. The cut removes only
    // subtrees without a witness, so the search finds the same first one.
    mpz_class reach = w + 1;
    mpz_mul_2exp(reach.get_mpz_t(), reach.get_mpz_t(), depth);
    if (reach > n3[depth]) {
        return false;
    }
    const mpz_class ue = 2 * w; // even preimage (T(ue) = w).
    if (ue >= 1 && ue < n) {
        y = ue;
        L = dist + 1;
        return true;
    }
    const bool have_odd = mod3(w) == 2;
    mpz_class uo;
    if (have_odd) {
        uo = (2 * w - 1) / 3; // odd preimage (T(uo) = w).
        if (uo >= 1 && uo < n) {
            y = uo;
            L = dist + 1;
            return true;
        }
    }
    if (depth <= 1) {
        return false;
    }
    if (backward_merge_witness(ue, n, n3, depth - 1, dist + 1, y, L)) {
        return true;
    }
    return have_odd && backward_merge_witness(uo, n, n3, depth - 1, dist + 1, y, L);
}

/// True iff some window iterate T^s(n) (s in [0, maxk]) has a value-decreasing
/// backward preimage chain (depth <= chain_depth) whose ancestor y satisfies
/// 1 <= y < n and, walked forward, T^L(y) == T^s(n). This is the window
/// chain-merge certificate replayed on the candidate's concrete trajectory.
[[nodiscard]] bool
chain_reduces_below(const Prefix& p, const mpz_class& n, unsigned maxk, unsigned chain_depth) {
    std::vector<mpz_class> n3(static_cast<std::size_t>(chain_depth) + 1);
    n3[0] = n;
    for (unsigned d = 1; d <= chain_depth; ++d) {
        n3[d] = 3 * n3[d - 1];
    }
    for (unsigned s = 0; s <= maxk; ++s) {
        mpz_class y;
        unsigned L = 0;
        if (backward_merge_witness(p.m[s], n, n3, chain_depth, 0, y, L) &&
            forward_meets(y, L, p.m[s])) {
            return true;
        }
    }
    return false;
}

} // namespace

/// GMP replay of a bitvector / partial-OEE window prune of n: a descent,
/// path-merge or OEE target below n within M steps, else within the
/// `window_len` = M + B window, else (chain_depth > 0) a backward chain of at
/// most chain_depth steps from a window iterate to a smaller ancestor.
[[nodiscard]] Replay
replay_window(const mpz_class& n, unsigned M, unsigned window_len, unsigned chain_depth) {
    thread_local Prefix p;
    fill_prefix(p, n, window_len);
    if (reduces_below(p, n, M)) {
        return Replay::within_m;
    }
    if (reduces_below(p, n, window_len)) {
        return Replay::in_window;
    }
    if (chain_depth > 0 && chain_reduces_below(p, n, window_len, chain_depth)) {
        return Replay::chain;
    }
    return Replay::none;
}

} // namespace collatz::certify::detail
