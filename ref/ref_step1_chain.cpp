// The backward preimage chain search of the reference mod-3^t rule; see
// ref/ref_step1.cpp.

#include "ref_step1_internal.hpp"

#include <gmpxx.h>

namespace collatz::ref::detail {
namespace {

/// ratio(L1,v1) < ratio(L2,v2)  <=>  2^{L1} 3^{v2} < 2^{L2} 3^{v1}. Exponents
/// are small (L,v <= 2t), so 64-bit products are exact.
[[nodiscard]] bool ref_better_ratio(unsigned l1, unsigned v1, unsigned l2, unsigned v2) {
    auto term = [](unsigned l, unsigned v) -> unsigned long long {
        unsigned long long r = 1ULL << l;
        for (unsigned i = 0; i < v; ++i) {
            r *= 3ULL;
        }
        return r;
    };
    return term(l1, v2) < term(l2, v1);
}

} // namespace

/// Brute-force DFS of the backward preimage tree of the concrete value `value`
/// (== A_current for the real m), depth <= 2t and at most t odd steps, recording
/// the minimal-ratio chain that lands on a strictly smaller integer (tie-break:
/// smaller reached value). The definitional counterpart of
/// collatz::preimage::ChainTable, computed on the actual value instead of a
/// residue table.
void ref_explore(const mpz_class& value,
                 unsigned L,
                 unsigned v,
                 unsigned t,
                 unsigned depth_left,
                 RefChain& best) {
    if (v >= 1) {
        // ratio 2^L/3^v < 1  <=>  2^L < 3^v.
        bool ratio_lt_1 = false;
        {
            unsigned long long num = 1ULL << L;
            unsigned long long den = 1ULL;
            for (unsigned i = 0; i < v; ++i) {
                den *= 3ULL;
            }
            ratio_lt_1 = num < den;
        }
        if (ratio_lt_1) {
            const bool take = !best.found || ref_better_ratio(L, v, best.L, best.v) ||
                              (L == best.L && v == best.v && value < best.A);
            if (take) {
                best.found = true;
                best.L = L;
                best.v = v;
                best.A = value;
            }
        }
    }
    if (depth_left == 0) {
        return;
    }
    // Even preimage (always integral).
    ref_explore(2 * value, L + 1, v, t, depth_left - 1, best);
    // Odd preimage: value == 2 (mod 3) and v < t.
    if (v < t && mpz_class(value % 3) == 2) {
        ref_explore((2 * value - 1) / 3, L + 1, v + 1, t, depth_left - 1, best);
    }
}

} // namespace collatz::ref::detail
