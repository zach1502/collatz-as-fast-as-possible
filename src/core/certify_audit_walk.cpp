// The bookkeeping of certify::audit() and its GMP walk of a must-check
// candidate: the classes already checked, the failure list, and the walk with
// its max_excursion and record checks; see certify.cpp.

#include "certify_audit.hpp"
#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gmpxx.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace collatz::certify::detail {
namespace {

/// A candidate walk still at or above its start after this many steps is
/// reported instead of continued, so that the background audit always ends.
constexpr std::uint64_t kAuditWalkCap = std::uint64_t{1} << 20;

} // namespace

// True when r lies in an excluded class already checked (`seen`): such a
// residue takes the same path to the same exclusion, so it needs no new walk.
bool AuditCtx::covered(u128 r) const {
    const std::lock_guard<std::mutex> lk(seen_mtx);
    for (unsigned k = 2; k <= M; ++k) {
        if (!seen[k].empty() && seen[k].count(r & ((static_cast<u128>(1) << k) - 1u)) != 0) {
            return true;
        }
    }
    return false;
}

void AuditCtx::fail(
    AuditTally& t, std::string n0, std::string n, unsigned k, std::string why) const {
    ++t.failures_total;
    if (keep == 0) {
        return;
    }
    t.failures.push_back(Failure{std::move(n0), std::move(n), k, std::move(why)});
    if (t.failures.size() > keep && t.failures.size() - keep >= std::max<std::size_t>(keep, 64)) {
        std::sort(t.failures.begin(), t.failures.end(), failure_before);
        t.failures.resize(keep);
    }
}

// A must-check candidate: the run walks its M-step prefix and then its tail
// until it drops below cand, so the peak of that walk is at most the
// candidate's excursion in the run (a block-granular tail may walk a few
// steps more).
void AuditCtx::walk(u128 cand, u128 r, AuditTally& t) const {
    ++t.candidates_walked;
    const mpz_class n = mpz_from_u128(cand);
    mpz_class v = n;
    mpz_class peak = n;
    std::uint64_t steps = 0;
    while (steps < M || v >= n) {
        if (steps == kAuditWalkCap) {
            fail(t,
                 collatz::to_string(r),
                 collatz::to_string(cand),
                 M,
                 "walk: still at or above n after 2^20 steps");
            return;
        }
        step_T_inplace(v);
        ++steps;
        if (v > peak) {
            peak = v;
        }
    }
    if (have_max) {
        ++t.peaks_checked;
        if (peak > max_reported) {
            fail(t,
                 collatz::to_string(r),
                 collatz::to_string(cand),
                 M,
                 "walk: peak " + collatz::oracle::to_decimal(peak) +
                     " exceeds the run's max_excursion");
        }
    }
    if (have_thr && peak > threshold) {
        const auto it = std::lower_bound(
            listed.begin(), listed.end(), cand, [](const Listed& e, u128 x) { return e.n < x; });
        if (it != listed.end() && it->n == cand) {
            ++t.records_checked;
            if (it->peak < peak) {
                fail(t,
                     collatz::to_string(r),
                     collatz::to_string(cand),
                     M,
                     "records: the listed peak is below the walk's peak " +
                         collatz::oracle::to_decimal(peak));
            }
        } else if (!a.records_truncated || (!listed.empty() && cand < listed.back().n)) {
            // A truncated list holds every qualifying n up to its last entry.
            ++t.records_checked;
            fail(t,
                 collatz::to_string(r),
                 collatz::to_string(cand),
                 M,
                 "records: peak " + collatz::oracle::to_decimal(peak) +
                     " is above the record threshold but n is not in the record list");
        }
    }
}

} // namespace collatz::certify::detail
