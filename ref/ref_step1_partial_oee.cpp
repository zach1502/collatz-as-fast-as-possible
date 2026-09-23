// The definitional high-bit partial-sieve threshold a* of the reference; see
// ref/include/collatz_ref/ref_step1.hpp.

#include "collatz/oracle.hpp" // to_mpz (GMP C-API string conversion)
#include "collatz/u128.hpp"
#include "collatz_ref/ref_step1.hpp"
#include "ref_step1_internal.hpp"

#include <cstdint>
#include <gmpxx.h>
#include <limits>
#include <vector>

namespace collatz::ref {

using namespace detail;

unsigned partial_oee_a_star(collatz::u128 n0_u, unsigned M, unsigned A, const Options& opts) {
    const std::uint32_t twoA = 1u << A;
    const mpz_class n0 = collatz::oracle::to_mpz(n0_u);
    mpz_class two_M = 1;
    mpz_mul_2exp(two_M.get_mpz_t(), two_M.get_mpz_t(), M); // 2^M

    // excl[a]: is the candidate n = n0 + a*2^M excluded by an enabled partial
    // certificate whose arm/depth completes by depth M?
    const auto excluded = [&](std::uint32_t a) -> bool {
        const mpz_class n = n0 + mpz_class(static_cast<unsigned long>(a)) * two_M;

        // Walk the M-step prefix of the true candidate, recording m_j and par[j].
        std::vector<mpz_class> mval(static_cast<std::size_t>(M) + 1);
        std::vector<unsigned char> par(M);
        mval[0] = n;
        for (unsigned j = 0; j < M; ++j) {
            par[j] = mpz_odd_p(mval[j].get_mpz_t()) ? 1U : 0U;
            mval[j + 1] = step_T(mval[j]);
        }

        // Descent: some depth k in [1, M] with m_k < n.
        if (opts.descent) {
            for (unsigned k = 1; k <= M; ++k) {
                if (mval[k] < n) {
                    return true;
                }
            }
        }
        // Path-merge: m_k == 2 (mod 3) and (2 m_k - 1)/3 < n.
        if (opts.path_merge) {
            for (unsigned k = 1; k <= M; ++k) {
                if (mpz_class(mval[k] % 3) == 2) {
                    const mpz_class pred = (2 * mval[k] - 1) / 3;
                    if (pred < n) {
                        return true;
                    }
                }
            }
        }
        // OEE: odd m_j (j < M), l >= 1 odd steps then 2 even steps all within
        // [0, M), with (m_j - 1)/2 < n.
        if (opts.oee) {
            for (unsigned j = 0; j < M; ++j) {
                if (par[j] == 0) {
                    continue; // arm must start on an odd value.
                }
                unsigned e = j;
                while (e < M && par[e] != 0) {
                    ++e; // first even step at or after j (l = e - j >= 1).
                }
                if (e >= M || (e + 1u) >= M || par[e + 1u] != 0) {
                    continue; // need two even steps within [0, M).
                }
                const mpz_class half = (mval[j] - 1) / 2; // m_j odd -> exact
                if (half < n) {
                    return true;
                }
            }
        }
        return false;
    };

    // The excluded set is an up-set; return the smallest a starting the maximal
    // excluded suffix, or UINT32_MAX if nothing in [0, 2^A) is excluded.
    std::uint32_t a_star = twoA;
    for (std::uint32_t a = twoA; a-- > 0;) {
        if (excluded(a)) {
            a_star = a;
        } else {
            break;
        }
    }
    if (a_star >= twoA) {
        return static_cast<unsigned>(std::numeric_limits<std::uint32_t>::max());
    }
    return a_star;
}

} // namespace collatz::ref
