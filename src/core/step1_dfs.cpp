// The Step 1 DFS over residue classes and its root and task entry points.

#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "step1_internal.hpp"
#include "step1_path.hpp"
#include "step1_sieve.hpp"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace collatz::step1::detail {

namespace {

/// The recursive DFS. It accumulates into `st` (thread-local for a worker) and
/// calls the survivor sink. When `ctx.tasks` is set, a node at
/// `ctx.split_depth` is stored as a task instead of processed here.
template <class V, bool Fast>
void dfs(const Ctx& ctx,
         Stats& st,
         V n0,
         V m,
         unsigned k,
         unsigned f,
         const WatchT<V>& watch,
         const PrefixCarryT<V>& carry_in,
         ArmStackT<V>& arms,
         const WatchT<V>& awatch,
         unsigned target) {
    if constexpr (!std::is_same_v<V, u128>) {
        assert(ctx.tasks == nullptr && "the parallel prelude runs the u128 DFS");
    } else if (ctx.tasks != nullptr && k == ctx.split_depth) {
        Frame fr{};
        fr.n0 = n0;
        fr.m = m;
        fr.k = k;
        fr.f = f;
        fr.watch = watch;
        fr.carry = carry_in;
        fr.awatch = awatch;
        if (ctx.opts->track_oee_astar) {
            snapshot_arms(arms, fr);
        }
        ctx.tasks->push_back(fr);
        return;
    }

    const Options& opts = *ctx.opts;

    if (k >= 2) {
        // Descent: m = T^k(n0) < n0 certifies the whole class.
        if (m < n0) {
            if (!Fast && opts.on_exclude) {
                opts.on_exclude(n0, k, reason_descent);
            }
            return;
        }
        // m mod 3 gates both the path merge (== 2) and the mod-3^t chain sieve
        // (!= 0).
        const unsigned m_mod3 = static_cast<unsigned>(m % 3);
        // Path-Merging (section 2.3). m == 2 (mod 3) makes 2m-1 divisible by 3,
        // and the quotient (2m-1)/3 is odd with T((2m-1)/3) == m.
        if (m_mod3 == 2) {
            const V pred = (2 * m - 1) / 3;
            if (pred < n0) {
                if (!Fast && opts.on_exclude) {
                    opts.on_exclude(n0, k, reason_path_merge);
                }
                return;
            }
        }
        // Odd-Even-Even (section 2.4): the watch has just completed odd-run + 2
        // evens.
        if (watch.phase == Phase::done && ((watch.mj - 1) / 2) < n0) {
            if (!Fast && opts.on_exclude) {
                opts.on_exclude(n0, k, reason_oee);
            }
            return;
        }
        // mod-3^t preimage-chain merge (section 2.5): Path-Merging generalized to
        // deeper pure-odd and mixed backward chains keyed on m mod 3^t. It needs
        // m != 0 (mod 3): every backward preimage of a value == 0 (mod 3) is again
        // == 0 (mod 3), so no odd shrinking step exists and such residues have no
        // value-decreasing preimage chain. Skipping them loses nothing and saves
        // the m % 3^t modulo and table lookup on about 1/3 of nodes. The gate is
        // != 0, not == 2, because mixed chains also start at m == 1 (mod 3): the
        // even preimage 2m == 2 (mod 3) has an odd preimage.
        if ((Fast || ctx.chain != nullptr) && m_mod3 != 0 && mod3t_hit<V, Fast>(ctx, n0, m, k, f)) {
            if (!Fast && opts.on_exclude) {
                opts.on_exclude(n0, k, reason_mod3t);
            }
            return;
        }
    }

    // A completing OEE arm (the watch just reached odd-run + 2 evens) is recorded,
    // so the survivor gets the partial-OEE a* without a Step-2 walk of its prefix.
    // The run-start arm gives the whole odd run's min a* (a*(j) grows with j
    // within a run), so the single-arm watch finds the class minimum. The saved
    // marks restore the stack on backtrack, since arms apply to this node's
    // subtree only.
    const bool track = Fast || ctx.opts->track_oee_astar;
    const unsigned arm_mark = arms.n;
    // Arms with slope 2^{j+1} <= 3^{fj} exclude nothing (leaf_oee_astar skips
    // them), so they are not recorded. kArmCap bounds the arms of any path.
    if (track && awatch.phase == Phase::done &&
        (static_cast<V>(1) << (awatch.j + 1u)) > pow3v<V>(awatch.fj)) {
        assert(arms.n < kArmCap && "an M <= 76 path records at most 25 arms");
        arms.a[arms.n] = OeeArmT<V>{awatch.mj, n0, awatch.j, awatch.fj};
        ++arms.n;
    }

    // This node folds its slope and intercept into the prefix bound and passes it
    // to its children or survivor (one max per node; see step1::Node). `target` is
    // the emission depth (ctx.M), so the shift target-k stays >= 0.
    const PrefixCarryT<V> carry = prefix_fold(carry_in, m, k, f, target);

    if (k == target) {
        ++st.survivors;
        Node node{n0, m, f, carry.pmax, carry.smax};
        if (track) {
            node.oee_astar = leaf_oee_astar(arms, n0, target);
            arms.n = arm_mark;
        }
        emit_survivor(ctx, node);
        return;
    }

    // The low child keeps n0; the high child adds the top bit 2^k.
    const V bit = static_cast<V>(1) << k;
    const V v_high = m + pow3v<V>(f); // depth-k value of the high child's rep.

    // Case slicing: above the slice depth only the child whose bit k matches the
    // target pattern is taken, so a slice does no sibling work. The sieves on
    // this path still run; if a path node is excluded, the whole slice is empty,
    // as in the full run.
    const bool case_gate = ctx.case_depth != 0 && k < ctx.case_depth;
    const bool want_high = case_gate && (((ctx.case_low >> k) & 1u) != 0u);
    const bool want_low = case_gate && !want_high;
    // Children at the emission depth: one inlined loop (u64).
    if constexpr (std::is_same_v<V, std::uint64_t>) {
        if (k + 1 == target) {
            for (unsigned h = want_high ? 1u : 0u; h < (want_low ? 1u : 2u); ++h) {
                const V v = h != 0 ? v_high : m;
                const bool step_odd = (v & 1) != 0;
                const WatchT<V> cw = advance_watch(watch, k, m, f, step_odd, h != 0);
                const WatchT<V> acw =
                    track ? advance_watch(awatch, k, m, f, step_odd, h != 0) : awatch;
                leaf<V, Fast>(ctx,
                              st,
                              h != 0 ? n0 + bit : n0,
                              step_T(v),
                              k + 1,
                              f + (step_odd ? 1u : 0u),
                              cw,
                              carry,
                              arms,
                              acw);
            }
            if (track) {
                arms.n = arm_mark;
            }
            return;
        }
    }

    // Low child (new bit 0): value stepped is m.
    if (!want_high) {
        const bool step_odd = (m & 1) != 0;
        const WatchT<V> cw = advance_watch(watch, k, m, f, step_odd, /*high=*/false);
        const WatchT<V> acw =
            track ? advance_watch(awatch, k, m, f, step_odd, /*high=*/false) : awatch;
        dfs<V, Fast>(
            ctx, st, n0, step_T(m), k + 1, f + (step_odd ? 1u : 0u), cw, carry, arms, acw, target);
    }
    // High child (new bit 1): value stepped is m + 3^f.
    if (!want_low) {
        const bool step_odd = (v_high & 1) != 0;
        const WatchT<V> cw = advance_watch(watch, k, m, f, step_odd, /*high=*/true);
        const WatchT<V> acw =
            track ? advance_watch(awatch, k, m, f, step_odd, /*high=*/true) : awatch;
        dfs<V, Fast>(ctx,
                     st,
                     n0 + bit,
                     step_T(v_high),
                     k + 1,
                     f + (step_odd ? 1u : 0u),
                     cw,
                     carry,
                     arms,
                     acw,
                     target);
    }

    // This node's arm is popped (the children restored their own marks), so
    // the parent's other subtree sees the correct arm prefix.
    if (track) {
        arms.n = arm_mark;
    }
}

/// The default configuration (mod-3^t, a* tracking, no callback) runs a copy of
/// the u64 DFS with those options constant.
[[nodiscard]] bool fast_opts(const Ctx& ctx) {
    const Options& o = *ctx.opts;
    return ctx.chain != nullptr && o.track_oee_astar && !o.on_exclude;
}

template <class V, bool Fast>
void dfs_root_as(const Ctx& ctx, Stats& st) {
    ArmStackT<V> arms;
    dfs<V, Fast>(ctx,
                 st,
                 static_cast<V>(root_n0),
                 static_cast<V>(root_m),
                 root_k,
                 root_f,
                 WatchT<V>{},
                 prefix_seed<V>(ctx.M),
                 arms,
                 arm_watch_seed<V>(static_cast<V>(root_n0)),
                 ctx.M);
}

} // namespace

/// DFS from the root; the parallel prelude (tasks set) always runs in u128.
void dfs_root(const Ctx& ctx, Stats& st) {
    if (!ctx.u64 || ctx.tasks != nullptr) {
        dfs_root_as<u128, false>(ctx, st);
    } else if (fast_opts(ctx)) {
        dfs_root_as<std::uint64_t, true>(ctx, st);
    } else {
        dfs_root_as<std::uint64_t, false>(ctx, st);
    }
}

namespace {

template <class V, bool Fast>
void dfs_task_as(const Ctx& ctx, Stats& st, const Frame& fr) {
    ArmStackT<V> arms;
    seed_arms(fr, arms);
    const PrefixCarryT<V> carry{static_cast<V>(fr.carry.pmax), static_cast<V>(fr.carry.smax)};
    dfs<V, Fast>(ctx,
                 st,
                 static_cast<V>(fr.n0),
                 static_cast<V>(fr.m),
                 fr.k,
                 fr.f,
                 narrow_watch<V>(fr.watch),
                 carry,
                 arms,
                 narrow_watch<V>(fr.awatch),
                 ctx.M);
}

} // namespace

/// DFS of one deferred subtree (worker side).
void dfs_task(const Ctx& ctx, Stats& st, const Frame& fr) {
    if (!ctx.u64) {
        dfs_task_as<u128, false>(ctx, st, fr);
    } else if (fast_opts(ctx)) {
        dfs_task_as<std::uint64_t, true>(ctx, st, fr);
    } else {
        dfs_task_as<std::uint64_t, false>(ctx, st, fr);
    }
}

} // namespace collatz::step1::detail
