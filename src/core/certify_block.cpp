// Block mode: the GMP re-derivation of a Step-1 class exclusion at the two ends
// of the class inside the block, with one certificate for both; see
// certify.cpp.

#include "certify_internal.hpp"
#include "collatz/step1.hpp"

#include <algorithm>
#include <gmpxx.h>
#include <string>

namespace collatz::certify::detail {
namespace {

/// Block mode, mod-3^t: one backward preimage path applied to the ends' values
/// (wa, wb) = (T^k(lo), T^k(hi)) at once, depth_left more steps, odd steps
/// v <= vmax (an odd step needs w == 2 (mod 3) at both ends). Accepts the first
/// path whose ancestors satisfy 1 <= ya < lo and 1 <= yb < hi, storing them and
/// the path length L. With vmax <= f every intermediate residue mod 3, hence
/// the path's validity, is the same on the whole class, and ya, yb are the two
/// ends of an affine function of the member.
[[nodiscard]] bool chain_pair(const mpz_class& wa,
                              const mpz_class& wb,
                              const mpz_class& lo,
                              const mpz_class& hi,
                              unsigned L,
                              unsigned v,
                              unsigned vmax,
                              unsigned depth_left,
                              mpz_class& ya,
                              mpz_class& yb,
                              unsigned& Lout) {
    if (L > 0 && wa >= 1 && wa < lo && wb >= 1 && wb < hi) {
        ya = wa;
        yb = wb;
        Lout = L;
        return true;
    }
    if (depth_left == 0) {
        return false;
    }
    if (chain_pair(2 * wa, 2 * wb, lo, hi, L + 1, v, vmax, depth_left - 1, ya, yb, Lout)) {
        return true;
    }
    if (v < vmax && mod3(wa) == 2 && mod3(wb) == 2) {
        return chain_pair((2 * wa - 1) / 3,
                          (2 * wb - 1) / 3,
                          lo,
                          hi,
                          L + 1,
                          v + 1,
                          vmax,
                          depth_left - 1,
                          ya,
                          yb,
                          Lout);
    }
    return false;
}

} // namespace

/// Block mode: re-derive the Step-1 class exclusion (class n0 mod 2^k, `reason`)
/// at both ends of the class inside the block, lo = L + n0 and
/// hi = L + 2^N - 2^k + n0, with one certificate for both (same k, same OEE arm,
/// same backward chain). The class members share the k-step parity word, so
/// T^k, every arm value and every merge target is affine in the member, and a
/// certificate that holds at both ends holds on every member in between. `t` is
/// the mod-3^t depth. On failure `why` says what did not hold.
[[nodiscard]] bool block_class_cert(const mpz_class& lo,
                                    const mpz_class& hi,
                                    unsigned k,
                                    int reason,
                                    unsigned t,
                                    std::string& why) {
    const Prefix a = build_prefix(lo, k);
    const Prefix b = build_prefix(hi, k);
    if (a.par != b.par) {
        why = "the class's two block ends have different parity words";
        return false;
    }
    const mpz_class& ma = a.m[k];
    const mpz_class& mb = b.m[k];
    unsigned f = 0;
    for (unsigned i = 0; i < k; ++i) {
        f += a.par[i];
    }
    switch (reason) {
    case step1::reason_descent:
        if (ma < lo && mb < hi) {
            return true;
        }
        why = "descent: T^k(n) >= n at a block end";
        return false;
    case step1::reason_path_merge: {
        // f >= 1 keeps T^k(n) == 2 (mod 3) on the whole class.
        const auto pm = [](const mpz_class& m, const mpz_class& n) {
            if (mod3(m) != 2) {
                return false;
            }
            const mpz_class y = (2 * m - 1) / 3;
            return y < n && step_T_mpz(y) == m;
        };
        if (f >= 1 && pm(ma, lo) && pm(mb, hi)) {
            return true;
        }
        why = "path-merge: (2T^k(n)-1)/3 is not a smaller preimage at a block end";
        return false;
    }
    case step1::reason_oee: {
        // The two steps k-2, k-1 are even and close an odd run ending at k-3;
        // any arm j of that run whose y = (m_j - 1)/2 is below n at both ends.
        if (k >= 3 && a.par[k - 3] != 0 && a.par[k - 2] == 0 && a.par[k - 1] == 0) {
            unsigned run_start = k - 3;
            while (run_start > 0 && a.par[run_start - 1] != 0) {
                --run_start;
            }
            for (unsigned j = run_start; j <= k - 3; ++j) {
                const mpz_class ya = (a.m[j] - 1) / 2;
                const mpz_class yb = (b.m[j] - 1) / 2;
                if (ya < lo && yb < hi && forward_meets(ya, k - j - 1, ma) &&
                    forward_meets(yb, k - j - 1, mb)) {
                    return true;
                }
            }
        }
        why = "oee: no arm with (m_j-1)/2 < n and T^(k-j-1)(y) == T^k(n) at both block ends";
        return false;
    }
    case step1::reason_mod3t: {
        mpz_class ya;
        mpz_class yb;
        unsigned L = 0;
        if (chain_pair(ma, mb, lo, hi, 0, 0, std::min(f, t), 2 * t, ya, yb, L) &&
            forward_meets(ya, L, ma) && forward_meets(yb, L, mb)) {
            return true;
        }
        why = "mod3t: no backward chain (v <= min(f, t)) to y < n at both block ends";
        return false;
    }
    default:
        why = "unknown reason";
        return false;
    }
}

} // namespace collatz::certify::detail
