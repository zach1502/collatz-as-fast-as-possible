#ifndef COLLATZ_PREIMAGE_CHAIN_WINDOW_CHAIN_TABLE_HPP
#define COLLATZ_PREIMAGE_CHAIN_WINDOW_CHAIN_TABLE_HPP

// The window chain-merge gain table of the Step-2 dip (collatz/preimage_chain.hpp
// includes this file).

#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace collatz::preimage {

/// Odd-step cap of the window merge-template family (empty-w ancestor chains,
/// see the merge-template note below), deeper than the t <= max_t of the
/// mod-3^t sieve and the window chain terms. With 3^12 = 531441 classes the
/// WindowChainTable gain array is about 27 MB. D is at most 12, so
/// t = min(D, max_window_t) never exceeds 12.
inline constexpr unsigned max_window_t = 12;

// Window chain-merge dip terms. Folds the mod-3^t preimage-chain certificates
// into the Step-2 window dip at every window step k. At window step k the
// residue walk has value v_k = T^k(r) and f_k odd steps so far. When f_k >= o
// the 3-adic residue of the real iterate T^k(x) mod 3^o is fixed by the parity
// word alone (T^k(x) == c_k*2^{-k} == v_k (mod 3^o), word-determinism), so v_k's
// class mod 3^o equals that of T^k(x) and any admissible backward chain of that
// class is valid for the real candidate.
//
// For a chain with L total backward steps and v odd steps (ratio 2^L/3^v < 1),
// the ancestor A(x)=(2^L x - K)/3^v is a smaller integer that forward-reaches x,
// so the candidate merges below itself. Its contraction margin, by the rational
// bound 306 log2(3) < 485 (as for descent/PM/OEE), is
//
//     306(k - L) - 485(f_k - v)  =  A_k + (485 v - 306 L),   A_k = 306k - 485 f_k.
//
// The "gain" 485 v - 306 L depends only on the chain (not on k or f_k), so this
// table precomputes, for each class c mod 3^{t_w} and each odd-step cap o_max in
// [0, t_w], the best gain over admissible chains of class c with v <= o_max. At
// window step k the dip reads gain(v_k mod 3^{t_w}, min(f_k, t_w)); the cap
// keeps v <= f_k, as word-determinism requires. Path-Merge is the (L=1,v=1)
// chain with gain 485-306 = 179, so these terms include the PM term. The dip is
// a max over more terms as t_w grows, so it is non-decreasing in t_w and prunes
// at least as much.
//
// General merge templates reduce to (deeper) ancestor chains. A general merge
// template is a pair of parity words u (x's first i T-steps, o_u odd) and w
// (y's first j T-steps, o_w odd) with
// T^i(x) = T^j(y). Folded into the window dip at step k (y = v_k), its
// contraction term is
//
//     term = (306k - 485 f_k) - 306(i - j) + 485(o_u - o_w)
//          = A_{k+j} + (485 o_u - 306 i)          [since o_w = f_{k+j} - f_k]
//
// This is identical to the empty-w ancestor chain u (L = i total steps,
// v = o_u odd) applied at the later step k+j, whose gate o_u <= f_{k+j} is
// looser than the (u, w) gate o_u <= f_k (f_k <= f_{k+j}). The window dip
// visits every step and includes that chain term at k+j, so every j >= 1
// template is dominated: the only additional pruning comes from ancestor chains
// with odd depth o_u up to D (beyond the cap t_w of the window chain terms, 6 by
// default), i.e. a deeper t_w. The template fold therefore uses this same
// WindowChainTable with t = min(D, max_window_t) and BFS depth D, and dip()
// takes the max over both the window chain table (t_w) and this deeper table,
// so dip_D >= dip_{t_w} by construction (monotone). Soundness is the window
// chain argument above at the deeper cap.
class WindowChainTable {
public:
    /// Sentinel for "no admissible decreasing chain within the cap".
    static constexpr std::int32_t kNoGain = -1000000;

    /// Window chain-merge table: odd-step cap `t`, BFS depth 2*t (see the
    /// two-argument constructor).
    explicit WindowChainTable(unsigned t) : WindowChainTable(t, 2u * t) {}

    /// Odd-step cap `t` (<= max_window_t) and an explicit backward-preimage BFS
    /// `depth` (= the general merge template's max ancestor word length i). The
    /// window chain terms are the depth = 2*t case; the window-template fold
    /// uses t = min(D, max_window_t), depth = D so the recorded chains are
    /// exactly the ancestor words of length <= D (see the merge-template note
    /// above).
    WindowChainTable(unsigned t, unsigned depth) : t_(t), modulus_(pow3(t)) {
        assert(t >= 1 && t <= max_window_t);
        const auto count = static_cast<std::size_t>(modulus_);
        // gain_[c*(t_+1) + o] = best (485 v - 306 L) over admissible chains of
        // class c with 1 <= v <= o (o in [0, t_]); kNoGain when none.
        gain_.assign(count * (static_cast<std::size_t>(t_) + 1u), kNoGain);
        // A large, residue-preserving lift keeps every intermediate value a
        // positive integer while the mod-3 branch tests stay lift-independent
        // (decided at odd-step depth < v <= t, i.e. below 3^t), as in
        // ChainTable.
        const u128 lift = modulus_ << 30;
        // Indexed by v in [0, t_] with t_ <= max_window_t: a fixed-capacity array.
        BestV best_v{};
        for (u128 c = 0; c < modulus_; ++c) {
            std::fill(best_v.begin(), best_v.end(), kNoGain);
            const u128 x_rep = c + lift;
            explore(x_rep, x_rep, 0, 0, depth, best_v);
            // Prefix-max over v to get the "v <= o_max" gains.
            std::int32_t* row =
                &gain_[static_cast<std::size_t>(c) * (static_cast<std::size_t>(t_) + 1u)];
            std::int32_t running = kNoGain;
            row[0] = kNoGain;
            for (unsigned o = 1; o <= t_; ++o) {
                if (best_v[o] > running) {
                    running = best_v[o];
                }
                row[o] = running;
            }
        }
        residue_local_ = check_residue_local();
    }

    [[nodiscard]] unsigned t() const noexcept {
        return t_;
    }
    [[nodiscard]] u128 modulus() const noexcept {
        return modulus_;
    }

    /// Best dip-term gain (485 v - 306 L) for a value's class `residue` mod 3^t
    /// using admissible decreasing chains with v <= o_max (o_max clamped to t).
    /// Returns kNoGain (a large negative) when no such chain exists. A positive
    /// return means the term A_k + gain strictly beats the plain descent A_k.
    [[nodiscard]] std::int32_t gain(u128 residue, unsigned o_max) const noexcept {
        const unsigned o = o_max > t_ ? t_ : o_max;
        return gain_[static_cast<std::size_t>(residue) * (static_cast<std::size_t>(t_) + 1u) + o];
    }

    /// Residue locality ("the class-major premise"): gain(c, o) ==
    /// gain(c mod 3^o, o) for every class c < 3^t and every cap o in [0, t],
    /// i.e. the gain at cap o depends only on c mod 3^o. Checked exhaustively
    /// over the whole table when it is built (3^t (t+1) lookups, << the BFS).
    ///
    /// Why it holds for every (t, depth). gain(c, o) is the max of
    /// 485 v - 306 L over backward words (L steps, v odd, 1 <= v <= min(o, t),
    /// L <= depth, 2^L < 3^v) that are admissible from x_rep = c + 3^t 2^30.
    /// After a word prefix with L' steps and v' odd ones the value is
    /// y = (2^{L'} x - K')/3^{v'} (K' fixed by the prefix); the only test is
    /// y mod 3 == 2 before the (v'+1)-th odd step. If x == x' (mod 3^{v'+1})
    /// then 2^{L'} x - K' == 2^{L'} x' - K' (mod 3^{v'+1}), both are divisible
    /// by 3^{v'}, so y == y' (mod 3): the test agrees. Hence a word with v odd
    /// steps is admissible for x iff for x mod 3^v (its tests use x mod 3^j,
    /// j <= v; even steps are untested), and every word the cap o admits has
    /// v <= o, so each term -- and the max -- is a function of c mod 3^o. The
    /// lift is == 0 mod 3^t (so x_rep == c mod 3^v), keeps every value a
    /// positive integer, and stays < 2^(50 + depth) (no u128 overflow for
    /// depth <= 24). The Step-2 class-major dip builder relies on this
    /// (T^k(r) mod 3^{f_k} is fixed by r mod 2^k, and it looks up the gain
    /// at o = min(f_k, t)); it falls back to the per-residue walk if a table
    /// ever reports false.
    [[nodiscard]] bool residue_local() const noexcept {
        return residue_local_;
    }

private:
    [[nodiscard]] bool check_residue_local() const noexcept {
        const auto count = static_cast<std::size_t>(modulus_);
        for (unsigned o = 0; o <= t_; ++o) {
            const auto m = static_cast<std::size_t>(pow3(o));
            for (std::size_t c = 0; c < count; ++c) {
                if (gain(static_cast<u128>(c), o) != gain(static_cast<u128>(c % m), o)) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Per-odd-step-count best gain scratch (entries past t_ are never read).
    using BestV = std::array<std::int32_t, max_window_t + 1u>;

    /// Searches the backward preimage tree depth first from `value` and records,
    /// per exact odd-step count v, the best gain (485 v - 306 L) among ratio<1
    /// chains. Mirrors ChainTable::explore; x_rep does not enter the gain (K is
    /// not stored).
    void explore(
        u128 value, u128 x_rep, unsigned L, unsigned v, unsigned depth_left, BestV& best_v) const {
        if (v >= 1) {
            const u128 num = static_cast<u128>(1) << L; // 2^L
            const u128 den = pow3(v);                   // 3^v
            if (num < den) {                            // ratio < 1: a smaller ancestor
                const auto g =
                    485 * static_cast<std::int32_t>(v) - 306 * static_cast<std::int32_t>(L);
                if (g > best_v[v]) {
                    best_v[v] = g;
                }
            }
        }
        if (depth_left == 0) {
            return;
        }
        // Even preimage (always integral): value grows, ratio *2.
        explore(2 * value, x_rep, L + 1, v, depth_left - 1, best_v);
        // Odd preimage: only when value == 2 (mod 3) and v < t, so that the
        // residue mod 3^t still decides admissibility.
        if (v < t_ && static_cast<unsigned>(value % 3) == 2) {
            explore((2 * value - 1) / 3, x_rep, L + 1, v + 1, depth_left - 1, best_v);
        }
    }

    unsigned t_;
    u128 modulus_; ///< 3^t
    std::vector<std::int32_t> gain_;
    bool residue_local_ = false; ///< see residue_local().
};

} // namespace collatz::preimage

#endif // COLLATZ_PREIMAGE_CHAIN_WINDOW_CHAIN_TABLE_HPP
