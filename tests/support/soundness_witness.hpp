#ifndef COLLATZ_TESTS_SOUNDNESS_WITNESS_HPP
#define COLLATZ_TESTS_SOUNDNESS_WITNESS_HPP

// Theorem 4.4 soundness witness shared by tests/core/test_step2_soundness.cpp and
// integration/test_step2_wide_a.cpp (GMP ground truth, independent of the optimized sieve).
//
// descends_or_merges(n, steps): does n descend below itself, or merge with a
// strictly smaller number, within `steps` T-steps? It returns true iff at least
// one of these holds:
//   * descent:    T^s(n) < n for some 1 <= s <= steps.
//   * path-merge: v = T^s(n) == 2 (mod 3) and (2v-1)/3 < n, 0 <= s <= steps.
//   * OEE:        v = T^j(n) odd, (v-1)/2 < n, the odd run starting at j ends
//                 at e < steps, and T-step e+1 < steps is also even.
//
// Descent is tested as the walk proceeds: a descent anywhere makes the predicate
// true regardless of the other two clauses, so the walk stops there. The path-merge
// and OEE clauses see the full window whenever no descent occurs. The trajectory
// lives in a reused thread_local buffer (mpz assignment recycles limbs instead of
// allocating steps+1 values per call), and the mod-3 / halving tests use the GMP
// C API to avoid temporaries.

#include <cstddef>
#include <gmp.h>
#include <gmpxx.h>
#include <vector>

namespace collatz::test {

inline bool descends_or_merges(const mpz_class& n, unsigned steps) {
    thread_local std::vector<mpz_class> val;
    thread_local std::vector<unsigned char> par;
    thread_local mpz_class tmp;
    if (val.size() < static_cast<std::size_t>(steps) + 1u) {
        val.resize(static_cast<std::size_t>(steps) + 1u);
    }
    if (par.size() < steps) {
        par.resize(steps);
    }
    mpz_set(val[0].get_mpz_t(), n.get_mpz_t());
    for (unsigned s = 0; s < steps; ++s) {
        mpz_srcptr v = val[s].get_mpz_t();
        mpz_ptr next = val[s + 1u].get_mpz_t();
        const bool odd = mpz_odd_p(v) != 0;
        par[s] = odd ? 1u : 0u;
        if (odd) {
            // T(v) = (3v+1)/2 (exact: 3v+1 is even for odd v).
            mpz_mul_ui(next, v, 3u);
            mpz_add_ui(next, next, 1u);
            mpz_tdiv_q_2exp(next, next, 1u);
        } else {
            mpz_tdiv_q_2exp(next, v, 1u);
        }
        // Descent clause, checked as soon as T^{s+1}(n) exists.
        if (mpz_cmp(next, n.get_mpz_t()) < 0) {
            return true;
        }
    }
    // Path-merge: v == 2 (mod 3) with (2v-1)/3 < n.
    for (unsigned s = 0; s <= steps; ++s) {
        mpz_srcptr v = val[s].get_mpz_t();
        if (mpz_fdiv_ui(v, 3u) == 2u) {
            mpz_mul_2exp(tmp.get_mpz_t(), v, 1u);
            mpz_sub_ui(tmp.get_mpz_t(), tmp.get_mpz_t(), 1u);
            mpz_tdiv_q_ui(tmp.get_mpz_t(), tmp.get_mpz_t(), 3u);
            if (mpz_cmp(tmp.get_mpz_t(), n.get_mpz_t()) < 0) {
                return true;
            }
        }
    }
    // OEE: odd v with (v-1)/2 < n, then the odd run ends at e and e+1 is even.
    for (unsigned j = 0; j < steps; ++j) {
        if (par[j] == 0u) {
            continue;
        }
        mpz_sub_ui(tmp.get_mpz_t(), val[j].get_mpz_t(), 1u);
        mpz_tdiv_q_2exp(tmp.get_mpz_t(), tmp.get_mpz_t(), 1u);
        if (!(mpz_cmp(tmp.get_mpz_t(), n.get_mpz_t()) < 0)) {
            continue;
        }
        unsigned e = j;
        while (e < steps && par[e] != 0u) {
            ++e;
        }
        if (e < steps && (e + 1u) < steps && par[e + 1u] == 0u) {
            return true;
        }
    }
    return false;
}

} // namespace collatz::test

#endif // COLLATZ_TESTS_SOUNDNESS_WITNESS_HPP
