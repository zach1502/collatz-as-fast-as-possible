// The checks of certify::audit() on the run's own figures
// (AuditOptions::run): its argmax and its record list walked again, and its
// counters and flags recomputed; see ReportedRun in collatz/certify.hpp.

#include "certify_audit.hpp"
#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/oracle.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gmpxx.h>
#include <string>
#include <utility>

namespace collatz::certify::detail {
namespace {

/// The excursion verify records for n: the largest T^j(n) before the orbit
/// first drops below n (step3::first_drop_peak, exact for every n).
[[nodiscard]] mpz_class excursion(u128 n) {
    std::string wide;
    const step3::Outcome o = step3::first_drop_peak(n, &wide);
    return o.max_exceeds_u128 ? mpz_class(wide, 10) : mpz_from_u128(o.max_seen);
}

[[nodiscard]] const char* yes_no(bool b) noexcept {
    return b ? "true" : "false";
}

} // namespace

void AuditCtx::check_run(AuditTally& t) const {
    const ReportedRun& run = *a.run;
    const u128 end = lift.L + block_width;
    const auto in_block = [this, end](u128 n) { return n >= lift.L && n < end; };
    const auto run_fail = [this, &t](u128 n, std::string why) {
        fail(t, "-", collatz::to_string(n), 0, std::move(why));
    };
    const auto dec = [](const mpz_class& x) { return collatz::oracle::to_decimal(x); };

    // The argmax: a walked member of the block whose excursion is the maximum.
    // A run that walked nothing reports 0 for both.
    const u128 top = run.max_excursion_n;
    if (run.candidates_checked == 0) {
        if (top != 0 || max_reported != 0) {
            run_fail(top, "max_excursion_n: the run walked no candidate but reports a maximum");
        }
    } else if (!in_block(top)) {
        run_fail(top, "max_excursion_n: not in the block");
    } else {
        ++t.excursions_recomputed;
        const mpz_class e = excursion(top);
        if (e != max_reported) {
            run_fail(top, "max_excursion_n: its excursion " + dec(e) + " is not max_excursion");
        }
    }

    // The record list, in the order the run wrote it (`listed` is sorted).
    if (!have_thr && !a.records.empty()) {
        run_fail(a.records.front().n, "records: listed without a record_threshold");
    }
    for (std::size_t i = 1; i < a.records.size(); ++i) {
        if (a.records[i].n <= a.records[i - 1].n) {
            run_fail(a.records[i].n, "records: not strictly ascending in n");
            break;
        }
    }
    for (const Listed& e : listed) {
        if (!in_block(e.n)) {
            run_fail(e.n, "records: n is not in the block");
            continue;
        }
        ++t.excursions_recomputed;
        const mpz_class x = excursion(e.n);
        if (x != e.peak) {
            run_fail(e.n, "records: the listed peak is not the excursion " + dec(x) + " of n");
        } else if (have_thr && e.peak <= threshold) {
            run_fail(e.n, "records: the listed peak is not above record_threshold");
        } else if (e.peak > max_reported) {
            run_fail(e.n, "records: the listed peak exceeds max_excursion");
        }
    }
    // Every walked n above the threshold is listed, the argmax too; a truncated
    // list holds only the smallest such n.
    if (have_thr && run.candidates_checked != 0 && in_block(top) && max_reported > threshold &&
        !(a.records_truncated && !listed.empty() && top > listed.back().n)) {
        const auto it = std::lower_bound(
            listed.begin(), listed.end(), top, [](const Listed& e, u128 x) { return e.n < x; });
        if (it == listed.end() || it->n != top) {
            run_fail(top, "records: max_excursion_n is above record_threshold but not listed");
        }
    }

    // The counters: Step 2 walks or prunes each of the 2^A extensions of a
    // survivor, and the survivors are residues 3 (mod 4) below 2^M.
    const u128 split = static_cast<u128>(run.candidates_checked) + run.step2_masked_out;
    const u128 extensions = static_cast<u128>(run.step1_survivors) << opts.A;
    if (split != extensions) {
        run_fail(lift.L,
                 "counters: candidates_checked + step2_masked_out = " + collatz::to_string(split) +
                     ", not step1_survivors * 2^A = " + collatz::to_string(extensions));
    }
    if (run.step1_survivors > (std::uint64_t{1} << (M - 2))) {
        run_fail(lift.L, "counters: step1_survivors exceeds 2^(M-2), the residues 3 (mod 4)");
    }

    // The flags, which verify derives from the maximum.
    verify::Result r;
    const bool exceeds = mpz_sizeinbase(max_reported.get_mpz_t(), 2) > 128;
    if (exceeds) {
        r.max_excursion = u128_max;
        r.max_excursion_wide = dec(max_reported);
        r.max_exceeds_u128 = true;
    } else {
        (void) collatz::from_string(dec(max_reported), r.max_excursion);
    }
    verify::Options vo;
    vo.N = opts.N;
    vo.A = opts.A;
    vo.B = opts.B;
    const bool exact =
        verify::result_max_exact(opts.N, opts.block_h, verify::unwalked_depth(vo), r);
    if (run.max_exact != exact) {
        run_fail(lift.L,
                 std::string("max_exact: the run says ") + yes_no(run.max_exact) +
                     ", its max_excursion gives " + yes_no(exact));
    }
    if (run.max_exceeds_u128 != exceeds) {
        run_fail(lift.L,
                 std::string("max_exceeds_u128: the run says ") + yes_no(run.max_exceeds_u128) +
                     ", its max_excursion gives " + yes_no(exceeds));
    }
}

} // namespace collatz::certify::detail
