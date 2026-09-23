#ifndef COLLATZ_CORE_CERTIFY_INTERNAL_HPP
#define COLLATZ_CORE_CERTIFY_INTERNAL_HPP

// Private declarations shared by the src/core/certify*.cpp pieces of the
// certificate audit: the u128 and GMP helpers, the step prefix of a candidate,
// the mod-9 certificate, the window replay, the option checks and the state of
// run() that both phases update.

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
#include <vector>

namespace collatz::certify::detail {

using collatz::step1::Node;
using collatz::step2::Filter;

// u128 helpers for the Step-1 certificate recompute, exact under the bound in
// the file comment of certify.cpp.

[[nodiscard]] inline u128 step_T_u128(u128 x) noexcept {
    return (x & 1u) != 0u ? (3u * x + 1u) >> 1 : x >> 1;
}

/// T^k(x), exact only for the Step-1 domain (the bound in the file comment).
[[nodiscard]] inline u128 iterate_T_u128(u128 x, unsigned k) noexcept {
    for (unsigned i = 0; i < k; ++i) {
        x = step_T_u128(x);
    }
    return x;
}

// GMP helpers for the Step-2 candidate replay. Every value they see is
// positive, so mpz_fdiv_ui(x, 3) is x mod 3.

[[nodiscard]] inline mpz_class step_T_mpz(const mpz_class& x) {
    if (mpz_odd_p(x.get_mpz_t()) != 0) {
        return (3 * x + 1) / 2;
    }
    return x / 2;
}

/// x <- T(x) in place, without temporaries.
inline void step_T_inplace(mpz_class& x) {
    mpz_ptr p = x.get_mpz_t();
    if (mpz_odd_p(p) != 0) {
        mpz_mul_ui(p, p, 3);
        mpz_add_ui(p, p, 1);
    }
    mpz_fdiv_q_2exp(p, p, 1);
}

[[nodiscard]] inline unsigned mod3(const mpz_class& x) {
    return static_cast<unsigned>(mpz_fdiv_ui(x.get_mpz_t(), 3));
}

/// Exact u128 -> mpz (two 64-bit limbs, least significant first).
[[nodiscard]] inline mpz_class mpz_from_u128(u128 v) {
    const std::uint64_t w[2] = {collatz::lo64(v), collatz::hi64(v)};
    mpz_class r;
    mpz_import(r.get_mpz_t(), 2, -1, sizeof(std::uint64_t), 0, 0, w);
    return r;
}

/// T^L(y) == w, checked by walking y forward L steps: a merge certificate
/// "y < n reaches the iterate w of n" needs only this forward walk.
[[nodiscard]] inline bool forward_meets(mpz_class y, unsigned L, const mpz_class& w) {
    for (unsigned i = 0; i < L; ++i) {
        step_T_inplace(y);
    }
    return y == w;
}

/// splitmix64 finalizer, for the deterministic replay sampler.
[[nodiscard]] inline std::uint64_t mix64(std::uint64_t z) noexcept {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// Precomputed M+B step prefix of a candidate: values m[0..len] and parities
/// par[0..len-1] (par[j] == 1 iff m[j] is odd).
struct Prefix {
    std::vector<mpz_class> m;
    std::vector<unsigned char> par;
};

/// Fills `p` with the len-step prefix of n; values already in `p` keep their
/// limb storage, so refilling a reused Prefix allocates little.
inline void fill_prefix(Prefix& p, const mpz_class& n, unsigned len) {
    p.m.resize(static_cast<std::size_t>(len) + 1);
    p.par.resize(len);
    p.m[0] = n;
    for (unsigned j = 0; j < len; ++j) {
        p.par[j] = (mpz_odd_p(p.m[j].get_mpz_t()) != 0) ? 1U : 0U;
        p.m[j + 1] = p.m[j];
        step_T_inplace(p.m[j + 1]);
    }
}

[[nodiscard]] inline Prefix build_prefix(const mpz_class& n, unsigned len) {
    Prefix p;
    fill_prefix(p, n, len);
    return p;
}

// Defined in certify_replay.cpp.
[[nodiscard]] bool reduces_below(const Prefix& p, const mpz_class& n, unsigned maxk);

// Defined in certify_block.cpp.
[[nodiscard]] bool block_class_cert(
    const mpz_class& lo, const mpz_class& hi, unsigned k, int reason, unsigned t, std::string& why);

/// mod-9 preimage certificate for n == 2,4,5,8 (mod 9), exact in u128
/// (n < 2^N <= 2^64, or n < 2^96 for a block, so 8n < 2^99 stays within u128).
/// n is a forward image of a strictly smaller number.
[[nodiscard]] inline bool mod9_cert(u128 n) noexcept {
    const unsigned r9 = static_cast<unsigned>(n % 9u);
    if (r9 == 2u || r9 == 5u || r9 == 8u) { // n == 2 (mod 3): T((2n-1)/3) == n.
        const u128 target = (2u * n - 1u) / 3u;
        return target < n && step_T_u128(target) == n;
    }
    if (r9 == 4u) { // T^3((8n-5)/9) == n.
        const u128 target = (8u * n - 5u) / 9u;
        return target < n && iterate_T_u128(target, 3) == n;
    }
    return false; // not a mod-9 exclusion.
}

/// Certificate the window replay finds for an excluded candidate.
enum class Replay { within_m, in_window, chain, none };

// Defined in certify_replay.cpp.
[[nodiscard]] Replay
replay_window(const mpz_class& n, unsigned M, unsigned window_len, unsigned chain_depth);

/// Backward-chain depth the replay searches: 2*t_w for the window chains and up
/// to D for the merge templates, but 0 when both are off.
[[nodiscard]] inline unsigned replay_chain_depth(const Options& opts) noexcept {
    return opts.window_chains > 0 || opts.window_templates_D > 0
               ? std::max(2u * opts.window_chains, opts.window_templates_D)
               : 0u;
}

// Defined in certify.cpp.
void validate_options(const Options& opts);
[[nodiscard]] step2::Params filter_params(const Options& opts, unsigned threads);

// An atomic counter: the enumeration calls on_exclude and the sink from several
// threads at once. Each counter has its own 128-byte block (one or two cache
// lines), so threads that bump different counters do not share a line.
// The per-extension counters are summed in locals of the sink and published
// once per survivor, which makes 2^A times fewer atomic updates. Sums commute,
// so the totals do not depend on the thread count.
struct alignas(128) Counter {
    std::atomic<std::uint64_t> v{0};
    void add(std::uint64_t d) noexcept {
        if (d != 0) {
            v.fetch_add(d, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t load() const noexcept {
        return v.load();
    }
};
static_assert(sizeof(Counter) == 128, "one counter per 128-byte line");

/// The state of one run(): the options, the filter and the counters that
/// on_exclude (Phase 1) and survivor (Phase 2) bump from the enumeration threads.
struct Auditor {
    Auditor(const Options& o, const Filter& f, const frontier::Lift& l, Report& r)
        : opts(o), filter(f), lift(l), rep(r), block(o.block_h != 0), M(o.N - o.A), fmin(f.fmin()),
          block_width(static_cast<u128>(1) << o.N), two_M(static_cast<u128>(1) << M),
          twoA(1u << o.A), window_len(M + o.B), chain_depth(replay_chain_depth(o)) {}

    const Options& opts;
    const Filter& filter;
    const frontier::Lift& lift;
    Report& rep;
    const bool block;
    const unsigned M;
    const unsigned fmin;
    const u128 block_width; // N <= 64
    const u128 two_M;
    const unsigned twoA;
    const unsigned window_len;
    const unsigned chain_depth;
    Counter a_descent, a_pm, a_oee, a_mod3t;
    Counter a_survivors, a_checked, a_excluded;
    Counter a_mod9, a_flt, a_window, a_replayed;
    Counter a_within, a_inwin, a_failures;
    Counter a_chain, a_block_checked;
    std::mutex fail_mtx;

    // Defined in certify_phase1.cpp.
    void
    record_fail(const std::string& n0, const std::string& n, unsigned k, const std::string& reason);
    void block_check(u128 n0, unsigned depth, int reason);
    void on_exclude(u128 n0, unsigned depth, int reason);

    // Defined in certify_phase2.cpp.
    void survivor(const Node& node_in);
};

} // namespace collatz::certify::detail

#endif // COLLATZ_CORE_CERTIFY_INTERNAL_HPP
