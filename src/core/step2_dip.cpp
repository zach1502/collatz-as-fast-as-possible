// Step 2 dip primitives: the per-residue dip walk (with the optional window
// terms), the parity mod-3 criterion, and inv(3^f) mod 2^B.

#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace collatz::step2 {

using namespace detail;

bool Filter::parity_mod3_is_2(std::uint64_t parity_bits, unsigned k) noexcept {
    if (k == 0) {
        return false;
    }
    const std::uint64_t window = (k >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << k) - 1);
    const std::uint64_t masked = parity_bits & window;
    if (masked == 0) {
        return false; // no odd step: arithmetic mod-3 not fixed by parities.
    }
    // Index of the last odd step in [0, k).
    const unsigned last_odd = 63u - static_cast<unsigned>(__builtin_clzll(masked));
    // Every step after the last odd one is even.
    const unsigned trailing_evens = (k - 1u) - last_odd;
    return (trailing_evens & 1u) == 0u; // even number of evens since last odd.
}

std::uint32_t Filter::dip(std::uint64_t m,
                          unsigned B,
                          const preimage::WindowChainTable* wct,
                          const preimage::WindowChainTable* wtt) {
    assert(B >= 2 && B <= max_B && "dip requires 2 <= B <= max_B");

    // The B-step parity sequence of the residue value. m < 2^B and
    // T^k(m) < 3^B (< 2^51 at B=32), so 64-bit arithmetic is exact.
    bool parity[max_B];           // parity[s] = step s took the odd branch.
    unsigned f_prefix[max_B + 1]; // f_prefix[k] = #odd among steps [0, k).
    std::uint64_t val[max_B + 1]; // val[k] = T^k(m) (< 3^B < 2^51), for the
                                  // window chain-merge class lookup.
    std::uint64_t parity_bits = 0;
    f_prefix[0] = 0;
    std::uint64_t v = m;
    val[0] = v;
    for (unsigned s = 0; s < B; ++s) {
        const bool odd = (v & 1u) != 0u;
        parity[s] = odd;
        if (odd) {
            parity_bits |= (std::uint64_t{1} << s);
        }
        f_prefix[s + 1] = f_prefix[s] + (odd ? 1u : 0u);
        v = odd ? (3u * v + 1u) / 2u : v / 2u;
        val[s + 1] = v;
    }

    // Window chain-merge modulus (3^{t_w}); 0/unused when wct is null.
    const std::uint64_t chain_mod =
        wct != nullptr ? static_cast<std::uint64_t>(wct->modulus()) : 0u;
    // Merge-template modulus (3^{min(D, max_window_t)}); 0 when off.
    const std::uint64_t tmpl_mod = wtt != nullptr ? static_cast<std::uint64_t>(wtt->modulus()) : 0u;

    std::int64_t best = 0; // A_0 = 0 is always in S_B(m).
    for (unsigned k = 0; k <= B; ++k) {
        const std::int64_t a_k = kHalveWeight * static_cast<std::int64_t>(k) -
                                 kOddWeight * static_cast<std::int64_t>(f_prefix[k]);
        if (a_k > best) {
            best = a_k;
        }
        // Path-merge (Lemma 2.8): needs >= 1 odd step and the parity mod-3
        // criterion.
        if (f_prefix[k] >= 1 && parity_mod3_is_2(parity_bits, k)) {
            const std::int64_t pm = a_k + (kOddWeight - kHalveWeight); // +179
            if (pm > best) {
                best = pm;
            }
        }
        // OEE: step k is odd, its odd run is followed by 2 even steps, all
        // within the window. The term is A_k + 306 (one extra halving).
        if (k < B && parity[k]) {
            unsigned e = k;
            while (e < B && parity[e]) {
                ++e; // first even step at or after k (l = e - k >= 1).
            }
            if (e < B && (e + 1u) < B && !parity[e + 1u]) {
                const std::int64_t oee = a_k + kHalveWeight; // +306
                if (oee > best) {
                    best = oee;
                }
            }
        }
        // Window chain-merge: the best admissible decreasing preimage chain of
        // val[k]'s class mod 3^{t_w}, restricted to odd-step count
        // v <= min(f_k, t_w) so word-determinism (f_k >= v) makes the residue
        // walk's class equal the real iterate's class mod 3^v. The dip term is
        // A_k + (485 v - 306 L); gain() returns that best (485 v - 306 L). This
        // generalizes the PM term (the (L=1,v=1) chain, gain 179) and can only
        // raise the max, so dip is monotone non-decreasing in t_w.
        if (wct != nullptr && f_prefix[k] >= 1) {
            const unsigned o_max = std::min<unsigned>(f_prefix[k], wct->t());
            const auto cls = static_cast<u128>(val[k] % chain_mod);
            const std::int32_t g = wct->gain(cls, o_max);
            if (g > 0) {
                const std::int64_t term = a_k + static_cast<std::int64_t>(g);
                if (term > best) {
                    best = term;
                }
            }
        }
        // Merge templates. As derived in preimage_chain/window_chain_table.hpp, a
        // (u, w) template's dip term equals an empty-w ancestor chain of odd depth
        // o_u <= D applied at this window step, so the fold reduces to the same
        // lookup against the deeper table `wtt` (t = min(D, max_window_t), BFS
        // depth D). The max over both tables keeps dip monotone: dip_D >= dip_{t_w}.
        // Soundness is the same (word-determinism gate o <= f_k).
        if (wtt != nullptr && f_prefix[k] >= 1) {
            const unsigned o_max = std::min<unsigned>(f_prefix[k], wtt->t());
            const auto cls = static_cast<u128>(val[k] % tmpl_mod);
            const std::int32_t g = wtt->gain(cls, o_max);
            if (g > 0) {
                const std::int64_t term = a_k + static_cast<std::int64_t>(g);
                if (term > best) {
                    best = term;
                }
            }
        }
    }
    return best < 0 ? 0u : static_cast<std::uint32_t>(best);
}

std::uint64_t Filter::inverse_pow3_mod2b(unsigned f, unsigned B) noexcept {
    assert(f <= pow3_max_exp && "inverse_pow3_mod2b: f beyond POW3 table");
    assert(B >= 1 && B <= 64 && "inverse_pow3_mod2b: B out of range");
    // 3^f mod 2^64 is the low limb of the exact 3^f (f <= 80 fits in u128).
    const std::uint64_t a = lo64(pow3(f));
    // Newton iteration for the inverse of an odd number mod 2^64: x is correct
    // to 1 bit initially (a is odd => x = 1 satisfies a*x == 1 mod 2), and each
    // step x <- x*(2 - a*x) doubles the number of correct low bits. Six steps
    // reach 64 bits (1,2,4,8,16,32,64), covering any B <= 64.
    std::uint64_t x = 1;
    for (int iter = 0; iter < 6; ++iter) {
        x = x * (2u - a * x);
    }
    const std::uint64_t mask = (B >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << B) - 1);
    return x & mask;
}

} // namespace collatz::step2
