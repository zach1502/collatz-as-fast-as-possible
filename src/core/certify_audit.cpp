// certify::audit(): the random audit of one block run, and the residues its
// seed draws; see certify.cpp.

#include "certify_audit.hpp"

#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/frontier.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/sha256.hpp"
#include "collatz/step1.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <gmpxx.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace collatz::certify::detail {

/// The order of AuditReport::failures: n ascending as a number, then k, reason
/// and n0.
[[nodiscard]] bool failure_before(const Failure& x, const Failure& y) {
    if (x.n.size() != y.n.size()) {
        return x.n.size() < y.n.size();
    }
    if (x.n != y.n) {
        return x.n < y.n;
    }
    if (x.k != y.k) {
        return x.k < y.k;
    }
    if (x.reason != y.reason) {
        return x.reason < y.reason;
    }
    return x.n0 < y.n0;
}

} // namespace collatz::certify::detail

namespace collatz::certify {

using namespace detail;

namespace {

/// False unless `s` is a non-empty run of decimal digits.
[[nodiscard]] bool parse_decimal(const std::string& s, mpz_class& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    return out.set_str(s, 10) == 0;
}

} // namespace

u128 audit_residue(u128 block_h, unsigned K, unsigned M, std::string_view seed, std::uint64_t i) {
    if (M < 3 || M > 65) {
        throw std::invalid_argument("certify audit: audit_residue needs M in [3, 65]");
    }
    Sha256 h;
    h.update("collatz-audit|");
    h.update(collatz::to_string(block_h));
    h.update("|");
    h.update(std::to_string(K));
    h.update("|");
    h.update(seed);
    h.update("|");
    h.update(std::to_string(i));
    const std::array<std::uint8_t, 32> d = h.finish();
    u128 x = 0;
    for (std::size_t b = 0; b < 16; ++b) {
        x = (x << 8) | static_cast<u128>(d[b]);
    }
    const u128 low = (static_cast<u128>(1) << (M - 2)) - 1u;
    return ((x & low) << 2) | 3u;
}

AuditReport audit(const Options& opts, const AuditOptions& a) {
    validate_options(opts);
    if (opts.block_h == 0) {
        throw std::invalid_argument("certify audit: needs a block (block_h > 0)");
    }
    const unsigned M = opts.N - opts.A;
    if (M < 3 || M > 63) {
        throw std::invalid_argument("certify audit: M = N - A must be in [3, 63]");
    }
    if (a.samples > kMaxAuditSamples) {
        throw std::invalid_argument("certify audit: samples must be at most " +
                                    std::to_string(kMaxAuditSamples));
    }
    const std::uint64_t samples = a.samples != 0 ? a.samples : audit_default_samples(M);
    const bool have_max = !a.max_excursion.empty();
    const bool have_thr = !a.record_threshold.empty();
    mpz_class max_reported;
    mpz_class threshold;
    if ((have_max && !parse_decimal(a.max_excursion, max_reported)) ||
        (have_thr && !parse_decimal(a.record_threshold, threshold))) {
        throw std::invalid_argument(
            "certify audit: max_excursion and record_threshold must be decimal");
    }
    if (a.run && !have_max) {
        throw std::invalid_argument("certify audit: the run's figures need its max_excursion");
    }
    std::vector<Listed> listed;
    listed.reserve(a.records.size());
    for (const ReportedRecord& r : a.records) {
        Listed e{r.n, mpz_class()};
        if (!parse_decimal(r.peak, e.peak)) {
            throw std::invalid_argument("certify audit: a record peak is not decimal");
        }
        listed.push_back(std::move(e));
    }
    std::sort(
        listed.begin(), listed.end(), [](const Listed& x, const Listed& y) { return x.n < y.n; });

    const auto t0 = std::chrono::steady_clock::now();
    const unsigned threads = opts.threads == 0 ? hardware_threads() : opts.threads;
    const frontier::Lift lift = frontier::make_lift(opts.block_h, opts.N, opts.A);
    const u128 block_width = static_cast<u128>(1) << opts.N; // N <= 64
    const Filter filter(filter_params(opts, threads));
    const unsigned fmin = filter.fmin();
    const unsigned twoA = 1u << opts.A;
    const unsigned window_len = M + opts.B;
    const unsigned chain_depth = replay_chain_depth(opts);

    std::vector<u128> cols(static_cast<std::size_t>(samples));
    for (std::uint64_t i = 0; i < samples; ++i) {
        cols[static_cast<std::size_t>(i)] = audit_residue(opts.block_h, opts.N, M, a.seed, i);
    }
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());

    // verify's Step-1 options. The case slice of depth M is the single path
    // of one residue (case_c = r >> 2, since r = 3 (mod 4)).
    step1::Options s1;
    s1.mod3t = opts.mod3t;
    s1.track_oee_astar = opts.partial_oee;
    s1.case_k = std::uint64_t{1} << (M - 2);
    // One mod-3^t table for every walk (an enumeration builds its own otherwise).
    const std::unique_ptr<const preimage::ChainTable> chains =
        opts.mod3t > 0 ? std::make_unique<const preimage::ChainTable>(opts.mod3t) : nullptr;
    s1.chain_table = chains.get();
    // Below this depth bound verify runs the u64 DFS without on_exclude, a
    // separate instantiation from the one that reports exclusions, so its
    // decision is taken from a walk without the callback. Its parallel prelude
    // runs the u128 DFS, so the audit walks each path with both.
    const bool fast_dfs = M <= step1::kU64MaxDepth;

    // Excluded classes already checked, by depth: a residue in one of them
    // takes the same path to the same exclusion, so it needs no new walk.
    std::mutex seen_mtx;
    std::vector<std::unordered_set<u128, U128Hash>> seen(M + 1);

    const std::size_t keep = kMaxFailures;

    // The checks the audit threads run, on the variables above.
    const AuditCtx ctx{.opts = opts,
                       .a = a,
                       .M = M,
                       .have_max = have_max,
                       .have_thr = have_thr,
                       .max_reported = max_reported,
                       .threshold = threshold,
                       .listed = listed,
                       .lift = lift,
                       .block_width = block_width,
                       .filter = filter,
                       .fmin = fmin,
                       .twoA = twoA,
                       .window_len = window_len,
                       .chain_depth = chain_depth,
                       .s1 = s1,
                       .fast_dfs = fast_dfs,
                       .seen_mtx = seen_mtx,
                       .seen = seen,
                       .keep = keep};

    std::vector<AuditTally> tallies = ctx.run_columns(cols, threads);
    if (a.run) {
        ctx.check_run(tallies.emplace_back());
    }

    AuditReport rep;
    rep.samples = samples;
    rep.columns = cols.size();
    rep.audited_numbers = static_cast<std::uint64_t>(cols.size()) << opts.A;
    rep.M = M;
    rep.L = lift.L;
    for (AuditTally& t : tallies) {
        rep.step1_classes += t.step1_classes;
        rep.survivors += t.survivors;
        rep.exclusions_checked += t.exclusions_checked;
        rep.candidates_walked += t.candidates_walked;
        rep.peaks_checked += t.peaks_checked;
        rep.records_checked += t.records_checked;
        rep.excursions_recomputed += t.excursions_recomputed;
        rep.failures_total += t.failures_total;
        rep.failures.insert(rep.failures.end(),
                            std::make_move_iterator(t.failures.begin()),
                            std::make_move_iterator(t.failures.end()));
    }
    std::sort(rep.failures.begin(), rep.failures.end(), failure_before);
    if (rep.failures.size() > keep) {
        rep.failures.resize(keep);
    }
    rep.audited_classes = rep.step1_classes + rep.survivors;
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

} // namespace collatz::certify
