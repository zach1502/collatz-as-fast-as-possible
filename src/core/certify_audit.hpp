#ifndef COLLATZ_CORE_CERTIFY_AUDIT_HPP
#define COLLATZ_CORE_CERTIFY_AUDIT_HPP

// Private declarations shared by the src/core/certify_audit*.cpp pieces of
// certify::audit(): a thread's tally, the parsed record list, the Step-1
// decision on a residue and the state that the column checks and the checks
// of the run's figures share.

#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/frontier.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <gmpxx.h>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace collatz::certify::detail {

// Defined in certify_audit.cpp.
[[nodiscard]] bool failure_before(const Failure& x, const Failure& y);

struct U128Hash {
    std::size_t operator()(u128 x) const noexcept {
        return static_cast<std::size_t>(mix64(collatz::lo64(x) ^ mix64(collatz::hi64(x))));
    }
};

/// One audit thread's counts, summed after the join.
struct AuditTally {
    std::uint64_t step1_classes = 0;
    std::uint64_t survivors = 0;
    std::uint64_t exclusions_checked = 0;
    std::uint64_t candidates_walked = 0;
    std::uint64_t peaks_checked = 0;
    std::uint64_t records_checked = 0;
    std::uint64_t excursions_recomputed = 0;
    std::uint64_t failures_total = 0;
    /// Holds the first `keep` failures in failure_before order once trimmed,
    /// so the merged list does not depend on which thread saw which failure.
    std::vector<Failure> failures;
};

/// An AuditOptions::records entry with its peak parsed.
struct Listed {
    u128 n;
    mpz_class peak;
};

// The Step-1 decision on residue r: a survivor node, or the excluded class
// (n0, k, reason) that `found` names.
struct Step1Decision {
    std::optional<Node> surv;
    bool found = false;
    u128 n0 = 0;
    unsigned k = 0;
    int reason = 0;
};

/// The variables audit() sets up, by reference or by value, and the checks its
/// threads run on them, one column of 2^A block members at a time. Several
/// threads call the member functions at once; seen_mtx guards `seen`.
struct AuditCtx {
    const Options& opts;
    const AuditOptions& a;
    const unsigned M;
    const bool have_max;
    const bool have_thr;
    const mpz_class& max_reported;
    const mpz_class& threshold;
    const std::vector<Listed>& listed;
    const frontier::Lift& lift;
    const u128 block_width;
    const Filter& filter;
    const unsigned fmin;
    const unsigned twoA;
    const unsigned window_len;
    const unsigned chain_depth;
    const step1::Options& s1;
    const bool fast_dfs;
    std::mutex& seen_mtx;
    std::vector<std::unordered_set<u128, U128Hash>>& seen;
    const std::size_t keep;

    // Defined in certify_audit_walk.cpp.
    [[nodiscard]] bool covered(u128 r) const;
    void fail(AuditTally& t, std::string n0, std::string n, unsigned k, std::string why) const;
    void walk(u128 cand, u128 r, AuditTally& t) const;

    // Defined in certify_audit_column.cpp.
    [[nodiscard]] Step1Decision walk_step1(u128 r, bool u128_dfs) const;
    void audit_column(u128 r, AuditTally& t) const;
    [[nodiscard]] std::vector<AuditTally> run_columns(const std::vector<u128>& cols,
                                                      unsigned threads) const;

    // Defined in certify_audit_run.cpp.
    void check_run(AuditTally& t) const;
};

} // namespace collatz::certify::detail

#endif // COLLATZ_CORE_CERTIFY_AUDIT_HPP
