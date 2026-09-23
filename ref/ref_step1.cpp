// Reference implementation of the Angeltveit "Step 1" sieve classification.
// See ref/include/collatz_ref/ref_step1.hpp for the specification.
//
// Everything runs on GMP big integers (mpz_class), recomputed for every start
// value. The trajectory m_0, m_1, ... is grown lazily and only as far as the
// first exclusion (or depth M), so numbers that descend quickly cost only a few
// big-integer steps.

#include "collatz_ref/ref_step1.hpp"

#include "collatz/oracle.hpp" // to_mpz / to_decimal (GMP C-API string conversion)
#include "collatz/u128.hpp"
#include "ref_step1_internal.hpp"

#include <cstdint>
#include <gmpxx.h>
#include <string>
#include <vector>

namespace collatz::ref {

using namespace detail;

namespace {

/// Converts a big integer known to fit in 128 bits into a u128 (exact, via a
/// decimal round-trip on the GMP C API). Used for survivor terminal nodes,
/// which stay well under 2^128 for the supported range.
[[nodiscard]] collatz::u128 to_u128(const mpz_class& v) {
    collatz::u128 out = 0;
    const std::string dec = collatz::oracle::to_decimal(v);
    const bool ok = collatz::from_string(dec, out);
    (void) ok; // fits by construction for M <= 26
    return out;
}

/// Internal classification result, carrying enough extra state to serve both
/// classify() (reason + depth), oee_witness() (the witness j), and survivors()
/// (terminal node m and odd-step count f, valid only when by == none).
struct Detail {
    Excl by = Excl::none;
    unsigned depth = 0;
    unsigned oee_j = 0;   ///< Valid only when by == oee.
    mpz_class final_m;    ///< Valid only when by == none: T^M(n0).
    unsigned final_f = 0; ///< Valid only when by == none: f_M.
};

/// Core classifier operating directly on the big-integer start value.
///
/// `odd[i]` records the parity of m_i (equivalently, "step i is odd"); the
/// history vectors `m` and `odd` grow lazily to the current depth so that an
/// early descent costs only a handful of steps.
[[nodiscard]] Detail classify_impl(const mpz_class& n0, unsigned M, const Options& opts) {
    Detail out;

    // Conventions: even n0 descends at depth 1, n0 == 1 (mod 4) at depth 2.
    if (mpz_even_p(n0.get_mpz_t())) {
        out.by = Excl::descent;
        out.depth = 1;
        return out;
    }
    if ((n0 % 4) == 1) {
        out.by = Excl::descent;
        out.depth = 2;
        return out;
    }

    // n0 == 3 (mod 4): iterate and test at each depth.
    std::vector<mpz_class> m;
    std::vector<unsigned char> odd;
    m.reserve(static_cast<std::size_t>(M) + 1);
    odd.reserve(static_cast<std::size_t>(M) + 1);
    m.push_back(n0);
    odd.push_back(1); // n0 is odd

    // Ensure m[0..idx] and odd[0..idx] are populated.
    const auto ensure = [&](unsigned idx) {
        while (m.size() <= idx) {
            mpz_class next = step_T(m.back());
            const unsigned char is_odd = mpz_odd_p(next.get_mpz_t()) ? 1U : 0U;
            m.push_back(std::move(next));
            odd.push_back(is_odd);
        }
    };

    for (unsigned k = 2; k <= M; ++k) {
        ensure(k); // computes m[2..k] (and thus m[k-1], m[k-2], m[k-3] too)

        // (1) Descent.
        if (opts.descent && m[k] < n0) {
            out.by = Excl::descent;
            out.depth = k;
            return out;
        }

        // (2) Path-merging.
        if (opts.path_merge && (m[k] % 3) == 2) {
            const mpz_class pred = (2 * m[k] - 1) / 3; // exact: m_k == 2 (mod 3)
            if (pred < n0) {
                out.by = Excl::path_merge;
                out.depth = k;
                return out;
            }
        }

        // (3) OEE: an all-odd run m_j..m_{k-3} followed by two even steps
        //     (m_{k-2}, m_{k-1}) whose "one-below" predecessor is < n0.
        //     Requires j in [2, k-3], hence k >= 5.
        if (opts.oee && k >= 5 && odd[k - 2] == 0 && odd[k - 1] == 0) {
            // Walk left from k-3 across the maximal odd run (bounded at index 2)
            // to find its start, then take the smallest witness j with
            // (m_j - 1)/2 < n0.
            unsigned run_start = k - 3;
            while (run_start > 2 && odd[run_start - 1] != 0) {
                --run_start;
            }
            if (odd[k - 3] != 0) { // the run must be non-empty (m_{k-3} odd)
                for (unsigned j = run_start; j <= k - 3; ++j) {
                    const mpz_class half = (m[j] - 1) / 2; // m_j odd -> exact
                    if (half < n0) {
                        out.by = Excl::oee;
                        out.depth = k;
                        out.oee_j = j;
                        return out;
                    }
                }
            }
        }

        // (4) mod-3^t preimage-chain merge. Find the best backward chain
        // for the concrete m_k and apply the (i)-(iii) soundness conditions.
        if (opts.mod3t > 0) {
            RefChain best;
            ref_explore(m[k], 0, 0, opts.mod3t, 2 * opts.mod3t, best);
            if (best.found) {
                unsigned fk = 0; // odd steps among steps 0..k-1
                for (unsigned i = 0; i < k; ++i) {
                    fk += odd[i];
                }
                // (i) f >= v, (ii) A < n0, (iii) 2^L * 3^{f-v} <= 2^k.
                if (fk >= best.v && best.A < n0) {
                    mpz_class slope = 1;
                    mpz_mul_2exp(slope.get_mpz_t(), slope.get_mpz_t(), best.L); // 2^L
                    mpz_class p3;
                    mpz_ui_pow_ui(p3.get_mpz_t(), 3, fk - best.v); // 3^{f-v}
                    slope *= p3;
                    mpz_class bound = 1;
                    mpz_mul_2exp(bound.get_mpz_t(), bound.get_mpz_t(), k); // 2^k
                    if (slope <= bound) {
                        out.by = Excl::mod3t;
                        out.depth = k;
                        return out;
                    }
                }
            }
        }
    }

    // Survivor: fill in the terminal node and the odd-step count.
    ensure(M);
    out.by = Excl::none;
    out.depth = M;
    out.final_m = m[M];
    unsigned f = 0;
    for (unsigned i = 0; i < M; ++i) {
        if (odd[i] != 0) {
            ++f;
        }
    }
    out.final_f = f;
    return out;
}

} // namespace

Classification classify(collatz::u128 n0, unsigned M, const Options& opts) {
    const Detail d = classify_impl(collatz::oracle::to_mpz(n0), M, opts);
    return Classification{d.by, d.depth};
}

bool oee_witness(
    collatz::u128 n0, unsigned M, const Options& opts, unsigned& out_j, unsigned& out_k) {
    const Detail d = classify_impl(collatz::oracle::to_mpz(n0), M, opts);
    if (d.by != Excl::oee) {
        return false;
    }
    out_j = d.oee_j;
    out_k = d.depth;
    return true;
}

std::vector<Survivor> survivors(unsigned M, const Options& opts) {
    std::vector<Survivor> result;
    const std::uint64_t total = std::uint64_t{1} << M;
    for (std::uint64_t v = 0; v < total; ++v) {
        // Values in [0, 2^M) fit a machine word; build the mpz directly rather
        // than via the (much slower) decimal round-trip in oracle::to_mpz.
        mpz_class n0_big;
        mpz_set_ui(n0_big.get_mpz_t(), static_cast<unsigned long>(v));
        const Detail d = classify_impl(n0_big, M, opts);
        if (d.by == Excl::none) {
            result.push_back(
                Survivor{static_cast<collatz::u128>(v), to_u128(d.final_m), d.final_f});
        }
    }
    return result;
}

} // namespace collatz::ref
