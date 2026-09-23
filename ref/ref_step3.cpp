// Reference Step-3 finisher and pow result digest term. See
// ref/include/collatz_ref/ref_step3.hpp. Value arithmetic in GMP; the digest
// mix is plain u64. It shares no code with src/core/step3_*.cpp.

#include "collatz_ref/ref_step3.hpp"

#include "collatz/oracle.hpp"

#include <cstdint>
#include <gmpxx.h>
#include <string>

namespace collatz::ref {
namespace {

/// Converts a non-negative mpz known to fit in 128 bits into a u128 (exact, via
/// a decimal round-trip on the GMP C API). Sets `overflow` if it does not fit.
[[nodiscard]] collatz::u128 mpz_to_u128(const mpz_class& v, bool& overflow) {
    collatz::u128 out = 0;
    const std::string dec = collatz::oracle::to_decimal(v);
    overflow = !collatz::from_string(dec, out);
    return out;
}

[[nodiscard]] mpz_class step_T(const mpz_class& x) {
    if (mpz_even_p(x.get_mpz_t())) {
        return x / 2;
    }
    return (3 * x + 1) / 2;
}

} // namespace

std::uint64_t mix64(std::uint64_t z) noexcept {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ULL;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return z;
}

namespace {
/// Rotate-left a 64-bit word; r must be in [1,63], since a shift by 64 is undefined.
[[nodiscard]] std::uint64_t rotl64(std::uint64_t x, unsigned r) noexcept {
    return (x << r) | (x >> (64u - r));
}
} // namespace

std::uint64_t
result_digest_term(collatz::u128 n, std::uint64_t blocks, collatz::u128 v_end) noexcept {
    // One splitmix64 finalizer over a 64-bit combination of the
    // (n, blocks, v_end) triple. Matches the CPU (collatz/digest_term.hpp) and
    // GPU (u128_limbs.h) terms.
    const std::uint64_t key = collatz::lo64(n) ^ rotl64(collatz::hi64(n), 29) ^
                              (blocks * 0xBF58476D1CE4E5B9ULL) ^
                              (collatz::lo64(v_end) * 0x94D049BB133111EBULL) ^
                              rotl64(collatz::hi64(v_end), 43);
    return mix64(key);
}

BlockResult finish_blocks(collatz::u128 n, collatz::u128 m) {
    BlockResult out;
    if (n <= 2) {
        out.blocks = 0;
        out.v_end = n; // base case: the trivial {1,2} cycle.
    } else {
        mpz_class v = collatz::oracle::to_mpz(m);
        const mpz_class n_mp = collatz::oracle::to_mpz(n);
        std::uint64_t blocks = 0;
        while (v >= n_mp) {
            // One 16-step block via the affine lemma: with r = v mod 2^16,
            // T^16(v) = T^16(r) + 3^{f16} * (v >> 16), f16 = odd steps of r's
            // 16-step block.
            const auto r = static_cast<std::uint64_t>(mpz_class(v % 65536).get_ui());
            std::uint64_t rr = r;
            unsigned f16 = 0;
            for (unsigned i = 0; i < 16; ++i) {
                if (rr & 1u) {
                    rr = (3 * rr + 1) / 2;
                    ++f16;
                } else {
                    rr /= 2;
                }
            }
            mpz_class q;
            mpz_fdiv_q_2exp(q.get_mpz_t(), v.get_mpz_t(), 16); // v >> 16
            mpz_class p3;
            mpz_ui_pow_ui(p3.get_mpz_t(), 3, f16); // 3^{f16}
            v = q * p3 + mpz_class(static_cast<unsigned long>(rr));
            ++blocks;
        }
        out.blocks = blocks;
        bool ovf = false;
        out.v_end = mpz_to_u128(v, ovf);
        out.overflow = ovf;
    }
    out.pow_term = result_digest_term(n, out.blocks, out.v_end);
    return out;
}

Finish finish(collatz::u128 n, collatz::u128 m_a) {
    Finish out;

    // Block recurrence (blocks, landing value and pow digest term) from m_a.
    const BlockResult br = finish_blocks(n, m_a);
    out.blocks = br.blocks;
    out.v_end = br.v_end;
    out.pow_term = br.pow_term;
    out.max_exceeds_u128 = br.overflow;

    // Exact maximum over the full trajectory from n until first drop < n. The
    // verifier's global max is over the full trajectory from the candidate
    // (prefix included), so the walk starts at n, not at m_a.
    const mpz_class n_mp = collatz::oracle::to_mpz(n);
    mpz_class x = n_mp;
    mpz_class mx = x;
    if (n > 1) {
        while (true) {
            x = step_T(x);
            if (x > mx) {
                mx = x;
            }
            if (x < n_mp) {
                break;
            }
        }
    }
    bool ovf = false;
    out.max_excursion = mpz_to_u128(mx, ovf);
    out.max_exceeds_u128 = out.max_exceeds_u128 || ovf;
    return out;
}

} // namespace collatz::ref
