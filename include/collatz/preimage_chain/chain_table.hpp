#ifndef COLLATZ_PREIMAGE_CHAIN_CHAIN_TABLE_HPP
#define COLLATZ_PREIMAGE_CHAIN_CHAIN_TABLE_HPP

// The mod-3^t preimage-chain table of the Step-1 sieve (collatz/preimage_chain.hpp
// includes this file).

#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace collatz::preimage {

/// Largest t of the table generator. 3^t stays small, and at search depth
/// D = 2t every intermediate value fits in u128.
inline constexpr unsigned max_t = 10;

/// The best (minimal-ratio) admissible backward chain for values == c (mod 3^t).
///
/// When `has_chain` is true the ancestor map is A(x) = (2^L * x - K) / 3^v for
/// any x == c (mod 3^t); it satisfies T^L(A(x)) = x, is a positive integer, and
/// (since the stored chain has ratio 2^L/3^v < 1) lands strictly below x. When
/// `has_chain` is false no value-decreasing odd-ancestor chain exists for c
/// (e.g. every c == 0 (mod 3), which has no odd preimage).
struct ChainEntry {
    bool has_chain = false;
    unsigned L = 0; ///< total backward steps (numerator exponent, 2^L)
    unsigned v = 0; ///< number of odd backward steps (denominator 3^v), 1 <= v <= t
    u128 K = 0;     ///< affine constant: A(x) = (2^L x - K)/3^v
};

/// Residue -> best-chain table for a fixed t, built by an exhaustive search of
/// the backward preimage tree to depth 2t. 3^t is small, so one table is built
/// and shared const across the enumeration.
class ChainTable {
public:
    explicit ChainTable(unsigned t) : t_(t), modulus_(pow3(t)) {
        assert(t >= 1 && t <= max_t);
        const auto count = static_cast<std::size_t>(modulus_);
        table_.assign(count, ChainEntry{});
        // A large, residue-preserving lift keeps every intermediate value a
        // positive integer while the mod-3 branch tests stay lift-independent
        // (they are decided at odd-step depth < v <= t, i.e. below 3^t).
        const u128 lift = modulus_ << 30;
        const unsigned depth = 2 * t;
        for (u128 c = 0; c < modulus_; ++c) {
            const u128 x_rep = c + lift;
            explore(x_rep, x_rep, 0, 0, depth, table_[static_cast<std::size_t>(c)]);
        }
    }

    [[nodiscard]] unsigned t() const noexcept {
        return t_;
    }
    [[nodiscard]] u128 modulus() const noexcept {
        return modulus_;
    }

    /// Best chain for a value's residue mod 3^t. `residue` must be < 3^t.
    [[nodiscard]] const ChainEntry& entry(u128 residue) const {
        return table_[static_cast<std::size_t>(residue)];
    }

private:
    /// ratio(L1,v1) < ratio(L2,v2)  <=>  2^{L1} 3^{v2} < 2^{L2} 3^{v1}. Values
    /// are small (L,v <= 2t <= 20), so u128 products never overflow.
    [[nodiscard]] static bool
    better_ratio(unsigned l1, unsigned v1, unsigned l2, unsigned v2) noexcept {
        const u128 lhs = (static_cast<u128>(1) << l1) * pow3(v2);
        const u128 rhs = (static_cast<u128>(1) << l2) * pow3(v1);
        return lhs < rhs;
    }

    /// Searches the backward preimage tree depth first from `value`
    /// (== A_current(x_rep)) with the running (L, v), and records in `best` the
    /// best node with v >= 1 and ratio < 1.
    void explore(u128 value,
                 u128 x_rep,
                 unsigned L,
                 unsigned v,
                 unsigned depth_left,
                 ChainEntry& best) const {
        if (v >= 1) {
            const u128 num = static_cast<u128>(1) << L; // 2^L
            const u128 den = pow3(v);                   // 3^v
            if (num < den) {                            // ratio < 1: a smaller ancestor
                // K = 2^L * x_rep - 3^v * value (exact, lift-independent).
                const u128 K = (x_rep << L) - den * value;
                const bool take = !best.has_chain || better_ratio(L, v, best.L, best.v) ||
                                  (L == best.L && v == best.v && K > best.K);
                if (take) {
                    best.has_chain = true;
                    best.L = L;
                    best.v = v;
                    best.K = K;
                }
            }
        }
        if (depth_left == 0) {
            return;
        }
        // Even preimage (always integral): value grows, ratio *2.
        explore(2 * value, x_rep, L + 1, v, depth_left - 1, best);
        // Odd preimage: only when value == 2 (mod 3) and v < t, so that the
        // residue mod 3^t still decides admissibility.
        if (v < t_ && static_cast<unsigned>(value % 3) == 2) {
            explore((2 * value - 1) / 3, x_rep, L + 1, v + 1, depth_left - 1, best);
        }
    }

    unsigned t_;
    u128 modulus_; ///< 3^t
    std::vector<ChainEntry> table_;
};

} // namespace collatz::preimage

#endif // COLLATZ_PREIMAGE_CHAIN_CHAIN_TABLE_HPP
