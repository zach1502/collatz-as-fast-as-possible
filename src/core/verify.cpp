// verify::run(). verify_internal.hpp declares the functions it calls in other
// files and names the file that defines each one.

#include "collatz/verify.hpp"

#include "collatz/step2.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "verify_internal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace collatz::verify {

using namespace detail;

namespace {

/// Reduce a 64-bit value modulo the Mersenne prime P = 2^61 - 1 using the fold
/// x = (x & P) + (x >> 61), which is exact for any x < 2^64 (x >> 61 <= 7, so a
/// single conditional subtract lands the result in [0, P)). Bit-for-bit equal
/// to `x % kCoveragePrime` but without the hardware 64-bit division.
[[nodiscard]] std::uint64_t mod_mersenne61(std::uint64_t x) noexcept {
    x = (x & kCoveragePrime) + (x >> 61);
    if (x >= kCoveragePrime) {
        x -= kCoveragePrime;
    }
    return x;
}

void check_step2_params(const Options& opts) {
    if (opts.A != 0) {
        // Validate the Step 2 parameters up front (the Filter constructor only
        // asserts, which is a no-op under NDEBUG).
        if (opts.A > step2::max_A || opts.B < 2 || opts.B > step2::max_B ||
            opts.num_bitvectors == 0 || opts.A >= opts.N) {
            throw std::invalid_argument("step2: parameters out of range");
        }
    }
}

// The Step-1 DFS keeps m = T^k(n0) < 3^M in u128, exact only while
// 3^M < 2^127 (with headroom for the pmax = m + smax bound), i.e. M <= 76.
// Beyond that the DFS value could overflow u128 silently, so such runs are
// rejected up front. The pipeline is exact (overflow_fallbacks resolved by the
// GMP oracle) for every N with N - A <= 76.
void check_dfs_bound(const Options& opts) {
    const unsigned M_dfs = opts.A < opts.N ? opts.N - opts.A : opts.N;
    if (M_dfs > 76) {
        throw std::invalid_argument("verify: N - A must be <= 76 (u128 DFS bound)");
    }
}

// Record list (Options::record_threshold): with threshold X every gate bar
// is capped at X + 1, so each candidate whose excursion exceeds X is
// reconstructed exactly and reported. An excursion above 2^128-1 always makes
// its candidate a contender (its tail max or prefix bound saturates to
// u128_max) and goes to offer_wide, so a threshold past u128 needs no cap.
// Returns whether the list is on; `rthr` must outlive the run.
bool setup_records(const Options& opts, RecordThreshold& rthr, std::vector<Acc>& accs) {
    const bool records_on = !opts.record_threshold.empty();
    if (records_on) {
        rthr.dec = opts.record_threshold;
        rthr.fits_u128 = collatz::from_string(rthr.dec, rthr.x);
        rthr.cap = opts.records_cap;
        const u128 cap = rthr.fits_u128 && rthr.x < u128_max ? rthr.x + 1 : u128_max;
        for (Acc& a : accs) {
            a.rt = &rthr;
            a.bar_cap = cap;
        }
    }
    return records_on;
}

// Deterministic merge of the per-thread accumulators.
void merge_accs(Result& res, const std::vector<Acc>& accs) {
    for (const Acc& a : accs) {
        res.candidates_checked += a.candidates;
        res.step2_masked_out += a.masked_out;
        res.step3_steps_total += a.steps_total;
        res.coverage_checksum =
            (res.coverage_checksum + mod_mersenne61_u128(a.coverage_raw)) % kCoveragePrime;
        // Reduce each thread's raw digest sum mod P once, then fold mod P (a ring
        // homomorphism, so thread-count independent).
        res.pow_checksum = (res.pow_checksum + mod_mersenne61_u128(a.pow_raw)) % kCoveragePrime;
        res.blocks_total += a.blocks_total; // diagnostic block-count sum
        res.overflow_fallbacks += a.overflow;
        res.prefix_walks += a.prefix_walks;
        if (a.max_exceeds) {
            res.max_exceeds_u128 = true;
        }
        if (!a.have_max) {
            continue;
        }
        if (a.maxv > res.max_excursion ||
            (a.maxv == res.max_excursion && a.maxn < res.max_excursion_n)) {
            res.max_excursion = a.maxv;
            res.max_excursion_n = a.maxn;
        }
    }
}

// An excursion above 2^128-1 beats every u128 record (which is saturated to
// u128_max for it), so the exact wide record decides max and argmax. Also
// gathers the record list.
void merge_wide_records(Result& res, std::vector<Acc>& accs, const Options& opts, bool records_on) {
    std::vector<std::pair<u128, u128>> raw_hits;
    std::vector<RecordHit> wide_hits;
    bool hits_dropped = false;
    for (Acc& a : accs) {
        if (!a.wide_v.empty() &&
            (res.max_excursion_wide.empty() || decimal_less(res.max_excursion_wide, a.wide_v) ||
             (a.wide_v == res.max_excursion_wide && a.wide_n < res.max_excursion_n))) {
            res.max_excursion_wide = a.wide_v;
            res.max_excursion_n = a.wide_n;
        }
        raw_hits.insert(raw_hits.end(), a.rec_hits.begin(), a.rec_hits.end());
        wide_hits.insert(wide_hits.end(),
                         std::make_move_iterator(a.rec_wide.begin()),
                         std::make_move_iterator(a.rec_wide.end()));
        hits_dropped = hits_dropped || a.rec_dropped;
    }
    if (!res.max_excursion_wide.empty()) {
        res.max_excursion = u128_max;
        res.max_exceeds_u128 = true;
    }
    if (records_on) {
        res.records = finish_records(std::move(raw_hits),
                                     std::move(wide_hits),
                                     opts.records_cap,
                                     hits_dropped,
                                     res.records_truncated);
    }
}

} // namespace

/// Since 2^61 == 1 (mod P), x == (x & P) + ((x >> 61) & P) + (x >> 122) (mod P);
/// the three 61-bit limbs sum to < 2^62, which mod_mersenne61 lands in [0, P).
/// Reducing the raw sum once is bit-identical to reducing every term (the fold
/// is a ring homomorphism).
std::uint64_t mod_mersenne61_u128(u128 x) noexcept {
    const std::uint64_t s = (lo64(x) & kCoveragePrime) +
                            (static_cast<std::uint64_t>(x >> 61) & kCoveragePrime) +
                            static_cast<std::uint64_t>(x >> 122);
    return mod_mersenne61(s);
}

Result run(const Options& opts) {
    check_step2_params(opts);
    Result res;
    if (opts.N < 2) {
        return res;
    }
    validate_block(opts);
    const bool block = opts.block_h != 0;
    check_dfs_bound(opts);

    const auto t_start = Clock::now();

    unsigned nthreads = opts.threads == 0 ? hardware_threads() : opts.threads;
    if (nthreads < 1) {
        nthreads = 1;
    }
    std::vector<Acc> accs(nthreads);
    RecordThreshold rthr;
    const bool records_on = setup_records(opts, rthr, accs);

    // SoA batched tail over kTailLanes lanes.
    const std::size_t lanes = kTailLanes;
    std::vector<TailBatch> batches(nthreads);

    const u128 preflimit = run_preflight(opts, block, nthreads, accs, res);

    // Step 1 enumeration; the sink finishes each survivor via Step 3. The
    // stable tid lets each worker fold into its own accumulator without locks.
    const unsigned M = opts.N - opts.A;
    // Carry the partial-OEE a* in the Step-1 DFS (Node::oee_astar), which the
    // Step-2 mask reads. It is on for the partial-OEE Step-2 path (A > 0,
    // partial_oee).
    step1::Options s1 = opts.step1;
    s1.track_oee_astar = opts.A > 0 && opts.partial_oee;
    const RunCtx ctx{opts, s1, accs, batches, preflimit, lanes, nthreads, M};
    step1::Stats st;
    if (opts.A == 0) {
        st = run_step1_only(ctx);
        res.seconds_step1_2 = st.seconds;
    } else {
        st = run_step2(ctx, res);
    }
    res.step1 = st;

    // pow_kernel_id tags the block width (16 steps) of the block counts that
    // pow_checksum folds.
    res.pow_available = true;
    res.pow_kernel_id = 16;

    merge_accs(res, accs);
    merge_wide_records(res, accs, opts, records_on);

    // Certify (or not) that the reported max is the exact global maximum
    // (for a block: over every walked candidate, with the block's own bound).
    res.max_exact = result_max_exact(opts.N, opts.block_h, unwalked_depth(opts), res);

    res.seconds_total = std::chrono::duration<double>(Clock::now() - t_start).count();
    return res;
}

} // namespace collatz::verify
