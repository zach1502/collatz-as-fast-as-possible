// Certificate audit; see collatz/certify.hpp.
//
// Phase 1 re-derives the Step-1 class exclusions in u128. A class
// representative n0 < 2^M has T^k(n0) < 2*3^M for all k <= M, so with N <= 64
// (validate_options refuses more) the 128-bit iteration and the 2^(2t) growth
// of the mod-3^t chain search stay exact without an overflow guard. Phase 2
// recomputes the descent or merge certificate of each Step-2 extension
// exclusion in GMP (mpz_class), so it needs no width bound. Block mode
// (Options::block_h > 0) also re-derives every Step-1 class exclusion in GMP at
// the class's two ends inside the block, and runs Phase 2 on the lifted
// survivors, which are the block members themselves. audit() applies the
// block-mode checks to a seeded random sample of residues mod 2^M instead of
// the whole tree, and also walks the sampled must-check candidates.
//
// certify_internal.hpp and certify_audit.hpp declare the functions that the
// pieces call across files and name the file that defines each one.

#include "collatz/certify.hpp"

#include "certify_internal.hpp"
#include "collatz/frontier.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <chrono>
#include <stdexcept>

namespace collatz::certify::detail {
namespace {

// Block mode: the preconditions of `verify --block` (A > 0, start >= 2^17,
// end <= 2^96, u128 lift headroom), checked by the same function.
void check_block_options(const Options& opts) {
    verify::Options vo;
    vo.N = opts.N;
    vo.block_h = opts.block_h;
    vo.A = opts.A;
    vo.B = opts.B;
    vo.num_bitvectors = opts.num_bitvectors;
    vo.partial_oee = opts.partial_oee;
    vo.step1.mod3t = opts.mod3t;
    vo.window_chains = opts.window_chains;
    vo.window_templates_D = opts.window_templates_D;
    verify::validate_block(vo);
}

} // namespace

/// The option checks of run() and audit(), before anything is built.
void validate_options(const Options& opts) {
    // M = N - A is unsigned, so N <= A gets its own message before the
    // subtraction can wrap.
    if (opts.N <= opts.A) {
        throw std::invalid_argument("certify: N must be greater than A (M = N - A >= 1)");
    }
    if (opts.A > step2::max_A || opts.B < 2 || opts.B > step2::max_B || opts.num_bitvectors == 0) {
        throw std::invalid_argument("certify: Step-2 parameters out of range");
    }
    // Phase 1 recomputes the class certificates in u128, exact only for
    // N <= 64 (the bound in the file comment), and step1::enumerate only
    // asserts its own u128 bound M <= 76 (a no-op under NDEBUG), which N <= 64
    // also keeps. Reject before building the filter.
    if (opts.N > 64) {
        throw std::invalid_argument("certify: N must be <= 64 (u128 certificate bound)");
    }
    if (opts.block_h != 0) {
        check_block_options(opts);
    }
}

/// The parameters of the look-ahead filter verify builds for these options.
[[nodiscard]] step2::Params filter_params(const Options& opts, unsigned threads) {
    step2::Params params;
    params.N = opts.N;
    params.A = opts.A;
    params.B = opts.B;
    params.num_bitvectors = opts.num_bitvectors;
    params.threads = threads;
    params.partial_oee = opts.partial_oee;
    params.window_chains = opts.window_chains;
    params.window_templates_D = opts.window_templates_D;
    params.use_bv_cache = opts.bv_cache;
    params.cache_only = opts.cache_only;
    return params;
}

} // namespace collatz::certify::detail

namespace collatz::certify {

using namespace detail;

Report run(const Options& opts) {
    validate_options(opts);

    const auto t0 = std::chrono::steady_clock::now();
    const unsigned M = opts.N - opts.A;
    const bool block = opts.block_h != 0;
    const frontier::Lift lift =
        block ? frontier::make_lift(opts.block_h, opts.N, opts.A) : frontier::Lift{};

    // The same look-ahead filter verify builds, shared read-only by all threads.
    const step2::Params params =
        filter_params(opts, opts.threads == 0 ? hardware_threads() : opts.threads);
    const Filter filter(params);
    const unsigned fmin = filter.fmin();

    Report rep;
    rep.M = M;
    rep.fmin = fmin;
    rep.block_h = opts.block_h;
    rep.L = lift.L;

    Auditor auditor(opts, filter, lift, rep);
    step1::Options s1;
    s1.mod3t = opts.mod3t;
    s1.track_oee_astar = opts.partial_oee; // Filter's OEE-only partial reads Node::oee_astar.
    s1.on_exclude = [&auditor](u128 n0, unsigned depth, int reason) {
        auditor.on_exclude(n0, depth, reason);
    };
    step1::enumerate_parallel(
        M, params.threads, s1, [&auditor](const Node& node_in) { auditor.survivor(node_in); });

    rep.s1_descent = auditor.a_descent.load();
    rep.s1_path_merge = auditor.a_pm.load();
    rep.s1_oee = auditor.a_oee.load();
    rep.s1_mod3t = auditor.a_mod3t.load();
    rep.s2_survivors = auditor.a_survivors.load();
    rep.s2_candidates_checked = auditor.a_checked.load();
    rep.s2_excluded_total = auditor.a_excluded.load();
    rep.s2_mod9 = auditor.a_mod9.load();
    rep.s2_f_lt_fmin = auditor.a_flt.load();
    rep.s2_window_total = auditor.a_window.load();
    rep.s2_window_replayed = auditor.a_replayed.load();
    rep.s2_window_within_m = auditor.a_within.load();
    rep.s2_window_in_window = auditor.a_inwin.load();
    rep.s2_window_chain = auditor.a_chain.load();
    rep.s1_block_checked = auditor.a_block_checked.load();
    rep.failures_total = auditor.a_failures.load();

    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

} // namespace collatz::certify
