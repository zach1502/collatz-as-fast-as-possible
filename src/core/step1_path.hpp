#pragma once

// Per-path helpers of the Step 1 DFS: the T step, the prefix bound, the
// odd-even-even watch and the stack of completing OEE arms.

#include "collatz/collatz_fn.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "step1_internal.hpp"

#include <cassert>
#include <type_traits>

namespace collatz::step1::detail {

/// One step of T. m stays below 3^M < 2^128 for M <= 64, so the step does not
/// overflow; the assert checks this in debug builds.
template <class V>
[[nodiscard]] static V step_T(V v) {
    if constexpr (std::is_same_v<V, u128>) {
        const StepResult r = collatz::T(v);
        assert(!r.overflow && "T overflowed u128; M must be <= 64");
        return r.value;
    } else {
        return (v & 1) != 0 ? v + (v >> 1) + 1 : v >> 1; // (3v+1)/2 without forming 3v+1
    }
}

/// 3^e as V (callers keep e <= M, so it fits).
template <class V>
[[nodiscard]] static V pow3v(unsigned e) {
    return static_cast<V>(pow3(e));
}

/// Seeds the carry with the depth j = 0 and j = 1 terms, which the DFS (rooted
/// at depth 2) never visits as nodes. Every survivor is n0 == 3 (mod 4),
/// so step 1 is odd: m_1 = T(1) = 2, f_1 = 1. j=0: s_0 = 2^M, intercept bound
/// m_0 + s_0 = 2^M. j=1: s_1 = 3*2^{M-1}, intercept bound 2 + 3*2^{M-1}.
template <class V>
[[nodiscard]] static PrefixCarryT<V> prefix_seed(unsigned M) {
    PrefixCarryT<V> pc;
    const V s0 = static_cast<V>(1) << M;
    const V s1 = static_cast<V>(3) << (M - 1);
    pc.smax = s0 > s1 ? s0 : s1;
    const V p0 = s0;
    const V p1 = static_cast<V>(2) + s1;
    pc.pmax = p0 > p1 ? p0 : p1;
    return pc;
}

/// Folds the depth-k node (value m_k, odd count f) into the carry: slope
/// s_k = 3^f * 2^{M-k} and intercept upper bound m_k + s_k. All terms stay below
/// 3^M < 2^128 for M <= 64 (as the node values do), so nothing overflows.
template <class V>
[[nodiscard]] static PrefixCarryT<V>
prefix_fold(const PrefixCarryT<V>& in, V m, unsigned k, unsigned f, unsigned M) {
    PrefixCarryT<V> out = in;
    const V s_k = pow3v<V>(f) << (M - k); // 3^f 2^{M-k} <= 3^M < 2^128
    if (s_k > out.smax) {
        out.smax = s_k;
    }
    const V p_k = m + s_k; // >= T^k(n0) for the full survivor (see Node docs)
    if (p_k > out.pmax) {
        out.pmax = p_k;
    }
    return out;
}

/// The root arm watch. Every survivor is n0 == 3 (mod 4), so steps 0 and 1 are
/// both odd, and at the DFS root (depth 2) the trajectory is inside an odd run
/// that starts at j = 0 (f_0 = 0, m_0 = n0). Seeded this way, the arm watch
/// detects the OEE arm whose run starts at step 0 or 1. The partial-OEE a* counts
/// those arms (j in [0, M)), but the Step-1 OEE sieve does not: it uses the
/// smallest witness j >= 2, as ref/ does. So the sieve `watch` must stay empty at
/// the root while the arm watch starts in the run.
template <class V>
[[nodiscard]] static WatchT<V> arm_watch_seed(V root_n0_val) {
    WatchT<V> w;
    w.phase = Phase::odd_run;
    w.j = 0;
    w.fj = 0;
    w.mj = root_n0_val;
    return w;
}

/// Advances the watch across one step from a parent node at depth `k` with value
/// `parent_m` (= T^k(n0_low)) and odd-step count `f`. `step_odd` is the parity
/// of the step actually taken; `high` selects the high child (new bit 1).
template <class V>
[[nodiscard]] static WatchT<V>
advance_watch(const WatchT<V>& w, unsigned k, V parent_m, unsigned f, bool step_odd, bool high) {
    WatchT<V> next = w;
    switch (w.phase) {
    case Phase::none:
    case Phase::done:
        // An odd step arms a new run at this depth; an even step leaves the watch
        // idle.
        if (step_odd) {
            next.phase = Phase::odd_run;
            next.j = k;
            next.fj = f;
            next.mj = parent_m; // low value T^k(n0_low); corrected below if high.
        } else {
            next.phase = Phase::none;
        }
        break;
    case Phase::odd_run:
        next.phase = step_odd ? Phase::odd_run : Phase::even1;
        break;
    case Phase::even1:
        if (step_odd) {
            // An odd step after the first even ends the old run and starts a new
            // one.
            next.phase = Phase::odd_run;
            next.j = k;
            next.fj = f;
            next.mj = parent_m;
        } else {
            next.phase = Phase::done;
        }
        break;
    }

    // Keeps mj = T^{next.j}(child_n0) when the high bit is added. The increment
    // 3^{fj} * 2^{k-j} is < 3^k < 2^128 and the sum stays below 3^k, so u128 does
    // not overflow for M <= 64 (see step1.hpp).
    if (high && next.phase != Phase::none) {
        assert(next.j <= k);
        next.mj += pow3v<V>(next.fj) << (k - next.j);
    }
    return next;
}

/// The min over the path's completing OEE arms of a*(j), evaluated at the full
/// leaf n0 (see OeeArm and step1::kOeeAStarNone): an extension a is excluded iff
/// (m_j^{(a)}-1)/2 < n = n0 + a*2^M, i.e. a*C > D with
/// C = 2^{M-j}(2^{j+1}-3^{f_j}) > 0 and D = m_j-1-2 n0; a*(j) = floor(D/C)+1 for
/// D >= 0 (D == 0 keeps a=0), else 0. Returns kOeeAStarNone when no arm has
/// positive slope (no OEE prune).
template <class V>
[[nodiscard]] static u128 leaf_oee_astar(const ArmStackT<V>& arms, V n0_leaf, unsigned M) {
    u128 best = kOeeAStarNone;
    for (unsigned i = 0; i < arms.n; ++i) {
        const OeeArmT<V>& arm = arms.a[i];
        const V p3 = pow3v<V>(arm.fj);
        const V two_j1 = static_cast<V>(1) << (arm.j + 1u);
        assert(two_j1 > p3 && "only positive-slope arms are recorded");
        const V delta = n0_leaf - arm.n0;                 // high bits, multiple of 2^{j+1}
        const V mj_leaf = arm.mj + p3 * (delta >> arm.j); // = T^j(n0_leaf)
        const V c_pos = (static_cast<V>(1) << (M - arm.j)) * (two_j1 - p3);
        const V d_lhs = mj_leaf - 1;
        const V d_rhs = 2 * n0_leaf;
        V a_star = 0;
        if (d_lhs >= d_rhs) {
            a_star = (d_lhs - d_rhs) / c_pos + 1;
        }
        if (a_star < best) {
            best = a_star;
        }
    }
    return best;
}

/// Copies the path's arm stack into a deferred frame for the worker (at most
/// kFrameArmCap arms; see kSplitDepth).
static inline void snapshot_arms(const ArmStack& src, Frame& fr) {
    assert(src.n <= kFrameArmCap && "a deferred frame holds at most kFrameArmCap arms");
    for (unsigned i = 0; i < src.n; ++i) {
        fr.arms[i] = src.a[i];
    }
    fr.narms = src.n;
}

/// Restores a worker's arm stack from a deferred frame.
template <class V>
static void seed_arms(const Frame& fr, ArmStackT<V>& dst) {
    dst.n = fr.narms;
    for (unsigned i = 0; i < fr.narms; ++i) {
        const OeeArm& a = fr.arms[i];
        dst.a[i] = OeeArmT<V>{static_cast<V>(a.mj), static_cast<V>(a.n0), a.j, a.fj};
    }
}

template <class V>
[[nodiscard]] static WatchT<V> narrow_watch(const Watch& w) {
    return WatchT<V>{w.phase, static_cast<V>(w.mj), w.fj, w.j};
}

} // namespace collatz::step1::detail
