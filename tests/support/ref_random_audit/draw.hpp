#ifndef COLLATZ_TESTS_REF_RANDOM_AUDIT_DRAW_HPP
#define COLLATZ_TESTS_REF_RANDOM_AUDIT_DRAW_HPP

// The draw of the ref/ audit: the draw ranges of a tier, one drawn
// configuration, its production Options and the independent max_exact
// predicate. ref_random_audit.hpp includes this file.

#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz_ref/ref_verify.hpp"

#include <algorithm>
#include <cstdint>
#include <gmpxx.h>
#include <sstream>
#include <string>

namespace collatz::test::audit {

/// Draw ranges for one sweep tier.
struct Tier {
    unsigned N_lo = 20;
    unsigned N_hi = 28;
    unsigned B_max = 20; ///< Largest B drawn (production cold build is O(2^B)).
    unsigned M_max = 22; ///< Largest N - A (ref Step 1 is O(2^M * M) GMP).
    unsigned mod3t_max = 6;
    unsigned tw_max = 10;
    unsigned D_max = 12;
};

/// One drawn configuration.
struct Cfg {
    unsigned N = 0, A = 0, B = 0, nbv = 0;
    bool partial_oee = false;
    unsigned mod3t = 0, tw = 0, D = 0, threads = 1;
};

inline std::string describe(const Cfg& c) {
    std::ostringstream os;
    os << "N=" << c.N << " A=" << c.A << " B=" << c.B << " nbv=" << c.nbv
       << " partial_oee=" << c.partial_oee << " mod3t=" << c.mod3t << " tw=" << c.tw << " D=" << c.D
       << " threads=" << c.threads;
    return os.str();
}

/// splitmix64: small, seedable, platform-independent (std distributions are
/// not reproducible across standard libraries).
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    unsigned uniform(unsigned lo, unsigned hi) { // inclusive
        return lo + static_cast<unsigned>(next() % (std::uint64_t{hi} - lo + 1));
    }
    bool chance(unsigned pct) {
        return next() % 100 < pct;
    }
};

inline Cfg draw(std::uint64_t seed, std::uint64_t index, const Tier& t) {
    Rng r{seed ^ (0xD1B54A32D192ED03ULL * (index + 1))};
    Cfg c;
    c.N = r.uniform(t.N_lo, t.N_hi);
    // A in [1, 14] with M = N - A in [3, M_max]; B > A and B < A both occur.
    const unsigned A_lo = c.N > t.M_max ? c.N - t.M_max : 1u;
    const unsigned A_hi = std::min(14u, c.N - 3u);
    c.A = r.uniform(std::max(1u, A_lo), A_hi);
    const unsigned band = r.uniform(0, 99);
    if (band < 20) {
        c.B = r.uniform(2, 5);
    } else if (band < 55) {
        c.B = r.uniform(6, 12);
    } else {
        c.B = r.uniform(13, t.B_max);
    }
    c.nbv = r.chance(30) ? 8u : r.uniform(1, 15);
    c.partial_oee = r.chance(70);
    c.mod3t = r.chance(50) ? 0u : r.uniform(1, t.mod3t_max);
    c.tw = r.chance(25) ? 0u : r.uniform(1, t.tw_max);
    c.D = r.chance(35) ? 0u : r.uniform(1, t.D_max);
    const unsigned th[3] = {1u, 3u, 8u};
    c.threads = th[r.uniform(0, 2)];
    return c;
}

inline verify::Options make_options(const Cfg& c) {
    verify::Options o;
    o.N = c.N;
    o.A = c.A;
    o.B = c.B;
    o.num_bitvectors = c.nbv;
    o.threads = c.threads;
    o.partial_oee = c.partial_oee;
    o.step1.mod3t = c.mod3t;
    o.window_chains = c.tw;
    o.window_templates_D = c.D;
    return o;
}

/// Independent max_exact predicate (defined in collatz/verify/bounds.hpp), from the
/// ref/ record.
inline bool expected_max_exact(const Cfg& c, const ref::VerifyResult& r) {
    if (c.N <= 17) {
        return true;
    }
    if (r.max_exceeds_u128 || r.max_excursion == 0) {
        return false;
    }
    const unsigned K = (c.N - c.A) + c.B;
    mpz_class lhs;
    mpz_class rhs;
    mpz_ui_pow_ui(lhs.get_mpz_t(), 3, K);
    mpz_mul_2exp(lhs.get_mpz_t(), lhs.get_mpz_t(), c.N);
    // Build the u128 record from four 32-bit limbs (portable: unsigned long may
    // be 32-bit on Windows).
    for (int shift = 96; shift >= 0; shift -= 32) {
        rhs <<= 32;
        rhs += static_cast<unsigned long>(static_cast<std::uint32_t>(r.max_excursion >> shift));
    }
    mpz_mul_2exp(rhs.get_mpz_t(), rhs.get_mpz_t(), K);
    return lhs < rhs;
}

} // namespace collatz::test::audit

#endif // COLLATZ_TESTS_REF_RANDOM_AUDIT_DRAW_HPP
