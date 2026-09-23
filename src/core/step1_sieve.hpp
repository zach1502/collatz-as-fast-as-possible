#pragma once

// The Step 1 node sieves (mod-3^t chains), survivor emission, the child-class
// count and the leaf-level node handler of the DFS.

#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/progress.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "step1_internal.hpp"
#include "step1_path.hpp"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace collatz::step1::detail {

[[gnu::noinline]] static void flush_buffer(const Ctx& ctx) {
    progress::survivors_found(ctx.buffer->n); // --progress-file
    (*ctx.batch_sink)(ctx.tid, ctx.buffer->v.data(), ctx.buffer->n);
    ctx.buffer->n = 0;
}

/// Emits one survivor: appends it to the per-thread buffer and flushes a full
/// batch (buffered path), or calls the per-survivor sink (unbuffered path).
[[gnu::always_inline]] static inline void emit_survivor(const Ctx& ctx, const Node& node) {
    if (ctx.buffer != nullptr) {
        EmitBuf& b = *ctx.buffer;
        b.v[b.n] = node;
        if (++b.n == ctx.batch_cap) {
            flush_buffer(ctx);
        }
        return;
    }
    (*ctx.sink)(node);
}

/// The mod-3^t preimage-chain sieve on node (n0, m, k, f). Returns true iff the
/// whole class n0 (mod 2^k) is excluded. See collatz/preimage_chain.hpp and
/// Options::mod3t for the (i)-(iii) soundness conditions.
template <class V>
[[nodiscard]] static bool
mod3t_excludes(const preimage::ChainTable& tbl, V n0, V m, unsigned k, unsigned f) {
    const preimage::ChainEntry& e = tbl.entry(m % static_cast<V>(tbl.modulus()));
    if (!e.has_chain) {
        return false;
    }
    // (i) residue uniformity across all extensions of the class.
    if (f < e.v) {
        return false;
    }
    // Ancestor A(m) = (2^L m - K)/3^v. m <= 3^M < 2^101 for M <= 64 and L is
    // small, so (m << L) stays within u128 (asserted); the u64 version retries in
    // u128 when it would not fit u64.
    if constexpr (!std::is_same_v<V, u128>) {
        if ((m >> (64u - e.L)) != 0) {
            return mod3t_excludes<u128>(tbl, n0, m, k, f);
        }
    } else {
        assert(bit_length(m) + static_cast<int>(e.L) < 128 && "mod3t: m<<L overflow");
    }
    const V numer = (m << e.L) - static_cast<V>(e.K);
    // (ii) base merge target strictly below the class representative:
    // floor(numer / 3^v) < n0  <=>  numer < n0 * 3^v (n0 * 3^v < 2^76 * 3^6).
    if (!(numer < n0 * pow3v<V>(e.v))) {
        return false;
    }
    // (iii) slope: 2^L * 3^{f-v} <= 2^k  <=>  3^{f-v} <= 2^{k-L}. The exponent
    // checks keep this from overflowing: if k < L or f-v exceeds the POW3 table,
    // the slope condition cannot hold.
    if (k < e.L) {
        return false;
    }
    const unsigned dexp = f - e.v;
    if (dexp > pow3_max_exp) {
        return false;
    }
    return pow3(dexp) <= (static_cast<u128>(1) << (k - e.L));
}

/// mod3t_excludes on the packed table with the conditions combined without
/// branching, so the only data-dependent branch is the rare exclusion.
[[gnu::always_inline]] static inline bool
mod3t_excludes_flat(const Ctx& ctx, std::uint64_t n0, std::uint64_t m, unsigned k, unsigned f) {
    const FlatChain& e = ctx.flat[static_cast<std::size_t>(m % ctx.flat_mod)];
    const unsigned L = e.L;
    if (((m >> 1u) >> (63u - L)) != 0) { // m << L would overflow (never at L = 0)
        return mod3t_excludes<u128>(*ctx.chain, n0, m, k, f);
    }
    const int d = static_cast<int>(f) - static_cast<int>(e.v);
    const unsigned idx = d > 0 ? static_cast<unsigned>(d) : 0u;
    const unsigned c1 = d >= 0 ? 1u : 0u;
    const unsigned c2 = (m << L) - e.K < n0 * e.p3v ? 1u : 0u;
    const unsigned c3 = static_cast<int>(k) - static_cast<int>(L) >= kNeedBits[idx] ? 1u : 0u;
    return (c1 & c2 & c3) != 0;
}

template <class V, bool Fast>
[[gnu::always_inline]] static inline bool
mod3t_hit(const Ctx& ctx, V n0, V m, unsigned k, unsigned f) {
    if constexpr (Fast) {
        return mod3t_excludes_flat(ctx, n0, m, k, f);
    } else {
        return mod3t_excludes(*ctx.chain, n0, m, k, f);
    }
}

/// A node at its emission depth: sieve, fold, emit. Inlined into the parent's
/// expansion so the leaf level makes no recursive calls.
template <class V, bool Fast>
[[gnu::always_inline]] static inline void leaf(const Ctx& ctx,
                                               Stats& st,
                                               V n0,
                                               V m,
                                               unsigned k,
                                               unsigned f,
                                               const WatchT<V>& watch,
                                               const PrefixCarryT<V>& carry_in,
                                               ArmStackT<V>& arms,
                                               const WatchT<V>& awatch) {
    const Options& opts = *ctx.opts;
    if (m < n0) {
        if (!Fast && opts.on_exclude) {
            opts.on_exclude(n0, k, reason_descent);
        }
        return;
    }
    const unsigned m_mod3 = static_cast<unsigned>(m % 3);
    if (m_mod3 == 2 && (2 * m - 1) / 3 < n0) {
        if (!Fast && opts.on_exclude) {
            opts.on_exclude(n0, k, reason_path_merge);
        }
        return;
    }
    if (watch.phase == Phase::done && ((watch.mj - 1) / 2) < n0) {
        if (!Fast && opts.on_exclude) {
            opts.on_exclude(n0, k, reason_oee);
        }
        return;
    }
    if ((Fast || ctx.chain != nullptr) && m_mod3 != 0 && mod3t_hit<V, Fast>(ctx, n0, m, k, f)) {
        if (!Fast && opts.on_exclude) {
            opts.on_exclude(n0, k, reason_mod3t);
        }
        return;
    }
    ++st.survivors;
    const PrefixCarryT<V> carry = prefix_fold(carry_in, m, k, f, k);
    Node node{n0, m, f, carry.pmax, carry.smax};
    if (Fast || opts.track_oee_astar) {
        const unsigned arm_mark = arms.n;
        if (awatch.phase == Phase::done &&
            (static_cast<V>(1) << (awatch.j + 1u)) > pow3v<V>(awatch.fj)) {
            assert(arms.n < kArmCap && "an M <= 76 path records at most 25 arms");
            arms.a[arms.n] = OeeArmT<V>{awatch.mj, n0, awatch.j, awatch.fj};
            ++arms.n;
        }
        node.oee_astar = leaf_oee_astar(arms, n0, k);
        arms.n = arm_mark;
    }
    emit_survivor(ctx, node);
}

} // namespace collatz::step1::detail
