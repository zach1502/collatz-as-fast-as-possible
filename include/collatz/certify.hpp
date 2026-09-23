#ifndef COLLATZ_CERTIFY_HPP
#define COLLATZ_CERTIFY_HPP

// Certificate audit.
//
// Re-derives, independently of the sieve, a descent or merge certificate for
// every exclusion the verifier makes at a given N: every excluded n must lead
// to a strictly smaller integer. (tests/integration/test_exclusion_certificates.cpp
// checks a 1-in-64 sample of the exclusions.)
//
// Phase 1 checks the Step-1 class exclusions and phase 2 the Step-2 extension
// exclusions, the two places where the verifier drops candidates.
//
//   Phase 1 (Step-1 class exclusions).  The residue-class DFS runs with
//   Options::on_exclude, and every excluded class (descent, path merge, OEE,
//   mod-3^t) has its certificate recomputed on the class representative n0.
//   n0 < 2^M gives T^k(n0) < 2*3^M for k <= M, so with N <= 64 (run() and
//   the CLI refuse more) u128 iteration and the 2^(2t) growth of the mod-3^t
//   chain search stay exact:
//     * descent    : T^k(n0) < n0.
//     * path-merge : m = T^k(n0) == 2 (mod 3), (2m-1)/3 < n0 and T((2m-1)/3)==m.
//     * OEE        : an odd arm m_j (j<=k-3) with the Hercher pattern and
//                    (m_j-1)/2 < n0, verified by the identity
//                    T^{k-j}(m_j) == T^{k-j-1}((m_j-1)/2).
//     * mod-3^t    : an independent backward-preimage-chain search on the
//                    concrete m = T^k(n0) finds a value-decreasing chain
//                    satisfying the mod-3^t conditions (i) f>=v, (ii) A(m)<n0,
//                    (iii) 2^L 3^{f-v} <= 2^k.
//
//   Phase 2 (Step-2 extension exclusions).  For every survivor and every
//   high-bit extension a that the verifier's look-ahead Filter excludes (mask
//   bit clear), the candidate n = n0 + a*2^M is certified in GMP:
//     * mod-9      : n == 2,4,5,8 (mod 9) is a forward image of a smaller
//                    number; every such exclusion is checked.
//     * f < fmin   : the whole class is pruned by Angeltveit's Theorem 4.4; a
//                    GMP walk finds T^k(n) < n within M steps; every such
//                    exclusion is checked.
//     * otherwise  : the bitvector or partial-OEE window prune. A GMP replay
//                    walks up to N-A+B steps and must find a descent below n or
//                    a path-merge or OEE merge target < n (the direction of
//                    Theorem 4.4). This is the costly check, run on 1 in
//                    `replay_sample` of these exclusions.
//
// A failing certificate (an n excluded without a smaller target) is a
// soundness bug and is recorded in Report::failures.
//
// Block mode (Options::block_h = h > 0) audits the block
// [L, L + 2^N), L = h * 2^N, the way `verify --block` runs it, on the concrete
// block members (GMP throughout):
//   Phase 1: besides the representative check above, every Step-1 class
//     exclusion (class n0 mod 2^k) is re-derived at both ends of the class
//     inside the block, lo = L + n0 and hi = L + 2^N - 2^k + n0, with one
//     certificate for both: descent T^k(n) < n; path-merge y = (2T^k(n)-1)/3
//     with y < n and T(y) == T^k(n); OEE y = (m_j-1)/2 < n with
//     T^(k-j-1)(y) == T^k(n); mod-3^t one backward chain (odd steps
//     v <= min(f, t), depth <= 2t) with 1 <= y < n and T^L(y) == T^k(n). The
//     members share the k-step parity word, so every target is affine in the
//     member and a certificate holding at both ends holds on the whole class.
//   Phase 2: each survivor is lifted into the block (frontier::lift_survivor)
//     before Filter::mask, as verify::run does, and every excluded extension
//     is re-derived on the block member n = L + n0 + a*2^M itself.
// In both modes the replay checks each merge target explicitly: it finds
// y < n and verifies T^L(y) == T^s(n) by walking y forward in GMP.

#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace collatz::certify {

/// Audit configuration. The defaults are the verifier's (A=10, mod3t=6,
/// partial_oee on); the CLI sets B to verify's CPU default for N.
struct Options {
    unsigned N = 38;             ///< Audit every exclusion for n < 2^N.
    unsigned A = 10;             ///< Step-2 high bits (verifier default).
    unsigned B = 28;             ///< Step-2 look-ahead width.
    unsigned num_bitvectors = 8; ///< Step-2 bitvector count.
    unsigned threads = 1;        ///< Worker threads (0 => hardware_threads()).
    unsigned mod3t = 6;          ///< mod-3^t Step-1 sieve (0 = off).
    bool partial_oee = true;     ///< High-bit partial OEE in Step 2.
    /// Window chain-merge dip terms (t_w); 6 is the verifier default. When > 0
    /// the Step-2 filter folds the backward preimage-chain certificates into the
    /// window dip, so more extensions are pruned, and the replay also accepts a
    /// chain certificate: a smaller ancestor y < n with T^L(y) == T^k(n), found
    /// by an independent GMP backward-preimage walk to depth 2*t_w.
    unsigned window_chains = 6;
    /// General merge-template dip terms (i+j<=D). 0 = off. When > 0 the
    /// replay's GMP backward-preimage walk must reach depth D (>= 2*t_w) to
    /// find the deeper ancestor chains the template dip relies on; see
    /// cmd_certify.
    unsigned window_templates_D = 0;
    /// Sample rate of the bitvector and partial window replay: 1 replays every
    /// such exclusion, K a deterministic 1 in K. Every mod-9 and f<fmin
    /// exclusion is certified regardless.
    unsigned replay_sample = 1;
    /// Frontier block mode: audit [block_h * 2^N, (block_h + 1) * 2^N) as
    /// `verify --block block_h --K N` runs it (see the file comment). 0 (the
    /// default) audits the full run [0, 2^N). The preconditions are those of
    /// verify::validate_block.
    u128 block_h = 0;
    /// Loads the Step-2 filter from the on-disk bitvector cache
    /// (step2::Params::use_bv_cache) instead of building it; a cache hit is
    /// bit-identical to a build. Off by default; tests set it to share the
    /// ctest-scoped cache.
    bool bv_cache = false;
    /// With bv_cache: fail (std::runtime_error) when the Step-2 tables are not
    /// in the cache instead of building them (`certify --audit-cache-only`,
    /// which the worker passes; the build needs 2^(B+1) bytes for the dip).
    bool cache_only = false;
    /// Test hooks that simulate a pipeline bug the audit must report (0 / false
    /// = off). test_drop_n: the block member (or, for a full run, the number)
    /// test_drop_n is treated as excluded even where the verifier's mask keeps it,
    /// as if the pipeline dropped a must-check candidate. test_unlifted_mask:
    /// block mode only, the Step-2 masks are computed from the unlifted
    /// survivor (the full-run residue) while the audit checks the block member,
    /// the hazard the survivor lift removes.
    u128 test_drop_n = 0;
    bool test_unlifted_mask = false;
};

/// Failures a report lists; failures_total counts every one.
inline constexpr std::size_t kMaxFailures = 64;

/// A certificate failure; every failure is a soundness bug.
struct Failure {
    std::string n0;     ///< Class representative (decimal).
    std::string n;      ///< Full candidate n = n0 + a*2^M (decimal), or n0 for Step 1.
    unsigned k = 0;     ///< Depth context of the reason.
    std::string reason; ///< Human-readable description.
};

/// Audit result. Every count is exact and independent of the thread count.
struct Report {
    // Phase 1: Step-1 class exclusions, all certified.
    std::uint64_t s1_descent = 0;
    std::uint64_t s1_path_merge = 0;
    std::uint64_t s1_oee = 0;
    std::uint64_t s1_mod3t = 0;
    /// Block mode: class exclusions re-derived at both ends of the class inside
    /// the block (equals the four counts above when every one was checked).
    std::uint64_t s1_block_checked = 0;

    // Phase 2: Step-2 extension exclusions.
    std::uint64_t s2_survivors = 0;          ///< Step-1 survivors audited.
    std::uint64_t s2_candidates_checked = 0; ///< Must-check (Step-3) extensions.
    std::uint64_t s2_excluded_total = 0;     ///< All pruned extensions.
    std::uint64_t s2_mod9 = 0;               ///< Pruned by mod-9 (all certified).
    std::uint64_t s2_f_lt_fmin = 0;          ///< Pruned by f<fmin (all certified).
    std::uint64_t s2_window_total = 0;       ///< Pruned by bitvector/partial window.
    std::uint64_t s2_window_replayed = 0;    ///< Of those, replayed in GMP.
    std::uint64_t s2_window_within_m = 0;    ///< Replayed & certified at depth <= M.
    std::uint64_t s2_window_in_window = 0;   ///< Replayed & certified in (M, M+B].
    std::uint64_t s2_window_chain = 0;       ///< Replayed & certified by a backward chain.

    std::vector<Failure> failures;    ///< Soundness failures (empty == clean).
    std::uint64_t failures_total = 0; ///< Total failures (may exceed failures.size()).

    unsigned M = 0;    ///< N - A.
    unsigned fmin = 0; ///< Step-2 fmin used.
    u128 block_h = 0;  ///< Options::block_h.
    u128 L = 0;        ///< Block start block_h * 2^N (0 for a full run).
    double seconds = 0.0;

    /// Fraction of window prunes replayed, in [0, 1], but 1.0 when there were none.
    [[nodiscard]] double replay_coverage() const noexcept {
        return s2_window_total == 0
                   ? 1.0
                   : static_cast<double>(s2_window_replayed) / static_cast<double>(s2_window_total);
    }
    [[nodiscard]] bool clean() const noexcept {
        return failures_total == 0;
    }
};

/// Runs the certificate audit. Throws std::invalid_argument for N > 64 (the
/// bound of the u128 Phase 1) and for Step-2 parameters out of range, as
/// verify::run does.
[[nodiscard]] Report run(const Options& opts);

/// Default sample count for large blocks.
inline constexpr std::uint64_t kDefaultAuditSamples = 400000;

/// Largest AuditOptions::samples audit() accepts.
inline constexpr std::uint64_t kMaxAuditSamples = 10000000;

/// Samples audit() draws when AuditOptions::samples is 0: kDefaultAuditSamples,
/// but at most 1/64 of the 2^(M-2) residues (at least 1024), so the audit of a
/// small block stays small next to the block. Requires M >= 3.
[[nodiscard]] constexpr std::uint64_t audit_default_samples(unsigned M) noexcept {
    const std::uint64_t share = M >= 66 ? kDefaultAuditSamples : (std::uint64_t{1} << (M - 2)) >> 6;
    return share < 1024 ? 1024 : share < kDefaultAuditSamples ? share : kDefaultAuditSamples;
}

/// One entry of the audited run's record list (verify::Result::records).
struct ReportedRecord {
    u128 n = 0;
    std::string peak; ///< decimal, any size
};

/// Figures of the audited run that audit() recomputes (AuditOptions::run, from
/// the run's `verify --json` with `certify --result`). Each is a failure when:
/// max_excursion_n is outside the block or its excursion (step3::first_drop_peak,
/// as verify records it) is not max_excursion; a listed record is out of order,
/// outside the block, not its n's excursion, at most record_threshold or above
/// max_excursion, or the list lacks max_excursion_n above the threshold (unless
/// truncated below it) or is not empty without a threshold; candidates_checked +
/// step2_masked_out != step1_survivors * 2^A, or step1_survivors > 2^(M-2);
/// max_exact or max_exceeds_u128 is not what max_excursion gives.
struct ReportedRun {
    u128 max_excursion_n = 0;
    std::uint64_t candidates_checked = 0;
    std::uint64_t step2_masked_out = 0;
    std::uint64_t step1_survivors = 0;
    bool max_exact = false;
    bool max_exceeds_u128 = false;
};

/// Random audit of one block run (`certify --block H --K K --audit-seed S`).
/// Draw i picks the residue r = audit_residue(H, K, M, seed, i), r = 3 (mod 4)
/// below 2^M, which stands for the 2^A block members
/// L + a*2^M + r. For each distinct r the Step-1 enumeration runs on r's path
/// alone (its case slice of depth M, with the options verify uses) and the
/// decision it makes is re-derived in GMP:
///   * r lies in an excluded class n0 (mod 2^k): the class certificate at both
///     ends inside the block, as block-mode run() checks it;
///   * r is a survivor: it is lifted and masked as verify does it, every
///     excluded extension gets its Step-2 certificate (mod 9, descent within M
///     steps for f < fmin, otherwise the full window replay), and every
///     must-check candidate is walked in GMP through its M-step prefix and on
///     until it drops below itself.
/// A walk's peak must not exceed max_excursion, and a walk whose peak exceeds
/// record_threshold must be in records with a peak at least as large.
///
/// Before the class or survivor decision is used, r's path is walked a second
/// time with the u128 DFS (step1::Options::force_u128) when M <= kU64MaxDepth,
/// and both walks must reach the same decision: verify's parallel prelude
/// decides the depths below its split depth (18) with the u128 DFS and its
/// tasks the deeper ones with the u64 DFS, so one walk alone would leave one of
/// the two unchecked.
struct AuditOptions {
    std::string seed;                    ///< any text; the worker passes the unit's result digest
    std::uint64_t samples = 0;           ///< residues to draw; 0: audit_default_samples(M)
    std::string max_excursion;           ///< the run's maximum (decimal); empty: not checked
    std::string record_threshold;        ///< decimal; empty: the run kept no record list
    std::vector<ReportedRecord> records; ///< ascending n
    bool records_truncated = false;      ///< the list holds only its smallest n
    std::optional<ReportedRun> run;      ///< needs max_excursion; nullopt: not checked
};

/// Result of audit(). Every count and the failure list depend on the options
/// and the seed only, not on the thread count.
struct AuditReport {
    std::uint64_t samples = 0;            ///< residues drawn
    std::uint64_t columns = 0;            ///< distinct residues among them
    std::uint64_t audited_classes = 0;    ///< distinct Step-1 classes they lie in
    std::uint64_t step1_classes = 0;      ///< of those, excluded classes
    std::uint64_t survivors = 0;          ///< of those, Step-1 survivors (the rest)
    std::uint64_t audited_numbers = 0;    ///< block members of the columns: columns * 2^A
    std::uint64_t exclusions_checked = 0; ///< certificates re-derived (classes + extensions)
    std::uint64_t candidates_walked = 0;  ///< must-check candidates walked in GMP
    std::uint64_t peaks_checked = 0;      ///< walks compared with max_excursion
    std::uint64_t records_checked = 0;    ///< walks above record_threshold looked up in records
    /// Reported excursions walked again: max_excursion_n and each listed record.
    std::uint64_t excursions_recomputed = 0;
    std::vector<Failure> failures;    ///< ascending n, at most kMaxFailures
    std::uint64_t failures_total = 0; ///< may exceed failures.size()
    unsigned M = 0;                   ///< N - A
    u128 L = 0;                       ///< block start
    double seconds = 0.0;

    [[nodiscard]] bool clean() const noexcept {
        return failures_total == 0;
    }
};

/// Residue of draw i: r = 4u + 3, where u is the first 16 bytes (big-endian) of
/// SHA-256("collatz-audit|H|K|S|i") reduced mod 2^(M-2), with H, K and i in
/// decimal and S the seed text. Throws std::invalid_argument unless
/// 3 <= M <= 65.
[[nodiscard]] u128
audit_residue(u128 block_h, unsigned K, unsigned M, std::string_view seed, std::uint64_t i);

/// Audit block opts.block_h at depth opts.N (see AuditOptions and ReportedRun).
/// Options::replay_sample is ignored: every exclusion of a sampled survivor is
/// replayed. Throws std::invalid_argument for a full run (block_h == 0), for
/// M = N - A outside [3, 63], for samples above kMaxAuditSamples, for a
/// non-decimal max_excursion, record_threshold or record peak, for `run`
/// without max_excursion, and for the parameters run() rejects; with
/// Options::cache_only, std::runtime_error when the tables are not cached.
[[nodiscard]] AuditReport audit(const Options& opts, const AuditOptions& a);

} // namespace collatz::certify

#endif // COLLATZ_CERTIFY_HPP
