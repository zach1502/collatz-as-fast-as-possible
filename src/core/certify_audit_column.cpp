// The column check of certify::audit(): the Step-1 decision on a residue, walked
// with the DFS that verify runs, and the re-derivation of every exclusion of its
// column; and the threads that run it over the drawn columns; see certify.cpp.

#include "certify_audit.hpp"
#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/frontier.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gmpxx.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace collatz::certify::detail {
namespace {

[[nodiscard]] bool same_decision(const Step1Decision& x, const Step1Decision& y) {
    if (x.surv || y.surv) {
        return x.surv && y.surv && x.surv->n0 == y.surv->n0 && x.surv->m == y.surv->m &&
               x.surv->f == y.surv->f && x.surv->pmax == y.surv->pmax &&
               x.surv->smax == y.surv->smax && x.surv->oee_astar == y.surv->oee_astar;
    }
    return x.found == y.found && x.n0 == y.n0 && x.k == y.k && x.reason == y.reason;
}

} // namespace

// Walks the case slice of residue r with verify's Step-1 options, with the
// u128 DFS when u128_dfs is set, and returns the decision it reaches.
Step1Decision AuditCtx::walk_step1(u128 r, bool u128_dfs) const {
    Step1Decision d;
    step1::Options o = s1;
    o.case_c = static_cast<std::uint64_t>(r >> 2);
    o.force_u128 = u128_dfs;
    const auto on_exclude = [&d](u128 c, unsigned depth, int why) {
        d.found = true;
        d.n0 = c;
        d.k = depth;
        d.reason = why;
    };
    const bool callback_now = !fast_dfs || u128_dfs;
    if (callback_now) {
        o.on_exclude = on_exclude;
    }
    (void) step1::enumerate(M, o, [&d](const Node& nd) { d.surv = nd; });
    if (!d.surv && !callback_now) {
        // The run's walk dropped r; the reporting walk names the class.
        bool emitted = false;
        o.on_exclude = on_exclude;
        (void) step1::enumerate(M, o, [&emitted](const Node&) { emitted = true; });
        if (emitted) {
            d.found = false;
        }
    }
    return d;
}

// Re-derives the Step-1 decision on residue r and the Step-2 decision on each
// of the 2^A block members of its column.
void AuditCtx::audit_column(u128 r, AuditTally& t) const {
    if (covered(r)) {
        return;
    }
    const Step1Decision d = walk_step1(r, false);
    if (fast_dfs && !same_decision(d, walk_step1(r, true))) {
        fail(t,
             collatz::to_string(r),
             collatz::to_string(lift.L + r),
             M,
             "step1: the u64 and u128 DFS decide this residue differently");
        return;
    }
    if (!d.surv) {
        if (!d.found) {
            fail(t,
                 collatz::to_string(r),
                 collatz::to_string(lift.L + r),
                 M,
                 "step1: the residue was dropped without a class exclusion on its path");
            return;
        }
        bool first = false;
        {
            const std::lock_guard<std::mutex> lk(seen_mtx);
            first = seen[d.k].insert(d.n0).second;
        }
        if (!first) {
            return; // checked by another thread meanwhile
        }
        const u128 lo = lift.L + d.n0;
        const u128 hi = lift.L + block_width - (static_cast<u128>(1) << d.k) + d.n0;
        std::string why;
        const bool ok =
            block_class_cert(mpz_from_u128(lo), mpz_from_u128(hi), d.k, d.reason, opts.mod3t, why);
        ++t.step1_classes;
        ++t.exclusions_checked;
        if (!ok) {
            fail(t, collatz::to_string(d.n0), collatz::to_string(lo), d.k, "block: " + why);
        }
        return;
    }

    ++t.survivors;
    Node node = *d.surv;
    frontier::lift_survivor(node, lift);
    // test_unlifted_mask (test hook): the decisions of the unlifted survivor.
    const step2::CandidateMask mask = filter.mask(opts.test_unlifted_mask ? *d.surv : node);
    for (unsigned av = 0; av < twoA; ++av) {
        const u128 cand = node.n0 + (static_cast<u128>(av) << M);
        // test_drop_n (test hook): a must-check candidate dropped as if excluded.
        const bool dropped = opts.test_drop_n != 0 && cand == opts.test_drop_n;
        if (mask.test(av) && !dropped) {
            walk(cand, r, t);
            continue;
        }
        ++t.exclusions_checked;
        const mpz_class n = mpz_from_u128(cand);
        if (node.f < fmin) {
            if (!reduces_below(build_prefix(n, M), n, M)) {
                fail(t,
                     collatz::to_string(r),
                     collatz::to_string(cand),
                     M,
                     "f<fmin: no descent within M steps");
            }
            continue;
        }
        if (mod9_cert(cand)) {
            continue;
        }
        if (replay_window(n, M, window_len, chain_depth) == Replay::none) {
            fail(t,
                 collatz::to_string(r),
                 collatz::to_string(cand),
                 window_len,
                 "window: no descent/merge within N-A+B steps");
        }
    }
}

// Runs audit_column on every residue of `cols` on at most `threads` threads,
// one tally per thread.
std::vector<AuditTally> AuditCtx::run_columns(const std::vector<u128>& cols,
                                              unsigned threads) const {
    std::atomic<std::size_t> next{0};
    const auto drain = [&](AuditTally& t) {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= cols.size()) {
                return;
            }
            audit_column(cols[i], t);
        }
    };
    const unsigned nthreads = static_cast<unsigned>(
        std::max<std::size_t>(1, std::min<std::size_t>(threads, cols.size())));
    std::vector<AuditTally> tallies(nthreads);
    if (nthreads == 1) {
        drain(tallies[0]);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) {
            pool.emplace_back([&drain, &tallies, t] { drain(tallies[t]); });
        }
        for (std::thread& th : pool) {
            th.join();
        }
    }
    return tallies;
}

} // namespace collatz::certify::detail
