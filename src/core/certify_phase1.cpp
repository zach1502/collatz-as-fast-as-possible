// Phase 1 of certify::run(): the u128 recompute of every Step-1 class
// exclusion and, in block mode, its GMP re-derivation at the two ends of the
// class inside the block; see certify.cpp.

#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

#include <mutex>
#include <string>

namespace collatz::certify::detail {
namespace {

/// ratio(L1,v1) < ratio(L2,v2), cross-multiplied so the test stays in integers:
/// 2^{L1} 3^{v2} < 2^{L2} 3^{v1}.
[[nodiscard]] bool better_ratio(unsigned l1, unsigned v1, unsigned l2, unsigned v2) noexcept {
    return (static_cast<u128>(1) << l1) * pow3(v2) < (static_cast<u128>(1) << l2) * pow3(v1);
}

struct RefChain {
    bool found = false;
    unsigned L = 0;
    unsigned v = 0;
    u128 A = 0;
};

/// Backward preimage-tree search on the concrete value `value`, written
/// independently of preimage::ChainTable and run on the actual m instead of a
/// residue. Records the minimal-ratio value-decreasing chain to depth 2t.
void explore_chain(
    u128 value, unsigned L, unsigned v, unsigned t, unsigned depth_left, RefChain& best) {
    if (v >= 1 && (static_cast<u128>(1) << L) < pow3(v)) { // ratio < 1: lands below `value`.
        const bool take = !best.found || better_ratio(L, v, best.L, best.v) ||
                          (L == best.L && v == best.v && value < best.A);
        if (take) {
            best = RefChain{true, L, v, value};
        }
    }
    if (depth_left == 0) {
        return;
    }
    explore_chain(2u * value, L + 1, v, t, depth_left - 1, best); // even preimage.
    if (v < t && static_cast<unsigned>(value % 3u) == 2u) {       // odd preimage.
        explore_chain((2u * value - 1u) / 3u, L + 1, v + 1, t, depth_left - 1, best);
    }
}

// Reconstructs the OEE witness: at depth k the two steps k-2, k-1 are even and
// close an odd run ending at k-3. Finds the smallest arm j in that run with
// (m_j - 1)/2 < n0 and checks the Hercher identity
// T^{k-j}(m_j) == T^{k-j-1}((m_j - 1)/2).
[[nodiscard]] bool oee_witness(u128 n0, unsigned k) {
    bool ok = false;
    if (k >= 5) {
        u128 mvals[130];
        unsigned char par[130];
        u128 v = n0;
        for (unsigned i = 0; i <= k; ++i) {
            mvals[i] = v;
            par[i] = (v & 1u) != 0u ? 1U : 0U;
            v = step_T_u128(v);
        }
        if (par[k - 2] == 0 && par[k - 1] == 0 && par[k - 3] != 0) {
            unsigned run_start = k - 3;
            while (run_start > 2 && par[run_start - 1] != 0) {
                --run_start;
            }
            for (unsigned j = run_start; j <= k - 3 && !ok; ++j) {
                const u128 target = (mvals[j] - 1u) / 2u;
                if ((mvals[j] & 1u) != 0u && target < n0 &&
                    iterate_T_u128(mvals[j], k - j) == iterate_T_u128(target, k - j - 1)) {
                    ok = true;
                }
            }
        }
    }
    return ok;
}

// Independent chain search on the concrete m = T^k(n0).
[[nodiscard]] bool mod3t_chain(u128 n0, unsigned depth, unsigned mod3t) {
    u128 v = n0;
    unsigned f = 0;
    for (unsigned i = 0; i < depth; ++i) {
        if ((v & 1u) != 0u) {
            ++f;
        }
        v = step_T_u128(v);
    }
    const u128 m = v; // T^depth(n0).
    RefChain best;
    explore_chain(m, 0, 0, mod3t, 2 * mod3t, best);
    bool ok = false;
    if (best.found && f >= best.v && best.A < n0) {
        // (iii) 2^L * 3^{f-v} <= 2^depth.
        const u128 slope = (static_cast<u128>(1) << best.L) * pow3(f - best.v);
        ok = slope <= (static_cast<u128>(1) << depth);
    }
    return ok;
}

} // namespace

void Auditor::record_fail(const std::string& n0,
                          const std::string& n,
                          unsigned k,
                          const std::string& reason) {
    a_failures.add(1);
    std::lock_guard<std::mutex> lk(fail_mtx);
    if (rep.failures.size() < kMaxFailures) {
        rep.failures.push_back(Failure{n0, n, k, reason});
    }
}

// Block mode, Phase 1: re-derive a class exclusion (n0 mod 2^k) at the
// class's first and last member inside the block (GMP).
void Auditor::block_check(u128 n0, unsigned depth, int reason) {
    if (!block) {
        return;
    }
    a_block_checked.add(1);
    const u128 lo = lift.L + n0;
    const u128 hi = lift.L + block_width - (static_cast<u128>(1) << depth) + n0;
    std::string why;
    if (!block_class_cert(mpz_from_u128(lo), mpz_from_u128(hi), depth, reason, opts.mod3t, why)) {
        record_fail(collatz::to_string(n0), collatz::to_string(lo), depth, "block: " + why);
    }
}

// Phase 1: certify one Step-1 class exclusion (u128, exact).
void Auditor::on_exclude(u128 n0, unsigned depth, int reason) {
    block_check(n0, depth, reason);
    switch (reason) {
    case step1::reason_descent: {
        a_descent.add(1);
        if (!(iterate_T_u128(n0, depth) < n0)) {
            record_fail(
                collatz::to_string(n0), collatz::to_string(n0), depth, "descent: T^k(n0) >= n0");
        }
        break;
    }
    case step1::reason_path_merge: {
        a_pm.add(1);
        const u128 m = iterate_T_u128(n0, depth);
        const bool ok = static_cast<unsigned>(m % 3u) == 2u && (2u * m - 1u) / 3u < n0 &&
                        step_T_u128((2u * m - 1u) / 3u) == m;
        if (!ok) {
            record_fail(collatz::to_string(n0),
                        collatz::to_string(n0),
                        depth,
                        "path-merge: (2m-1)/3 not a smaller preimage of m");
        }
        break;
    }
    case step1::reason_oee: {
        a_oee.add(1);
        const bool ok = oee_witness(n0, depth);
        if (!ok) {
            record_fail(collatz::to_string(n0),
                        collatz::to_string(n0),
                        depth,
                        "oee: no Hercher witness with target < n0");
        }
        break;
    }
    case step1::reason_mod3t: {
        a_mod3t.add(1);
        const bool ok = mod3t_chain(n0, depth, opts.mod3t);
        if (!ok) {
            record_fail(collatz::to_string(n0),
                        collatz::to_string(n0),
                        depth,
                        "mod3t: no admissible value-decreasing chain (i-iii)");
        }
        break;
    }
    default:
        record_fail(collatz::to_string(n0), collatz::to_string(n0), depth, "unknown reason");
        break;
    }
}

} // namespace collatz::certify::detail
