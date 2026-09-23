// The report lines of `collatz merge`: the merged counts, max_exact in
// verify's wording, and the records.

#include "cmd_merge_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace collatz::cli::cmd_merge {

namespace ckp = collatz::checkpoint;

void print_merged_counts(unsigned N, const collatz::verify::Result& r) {
    const double total = std::ldexp(1.0, static_cast<int>(N));
    const double fraction = static_cast<double>(r.candidates_checked) / total;
    std::printf("candidates_checked = %llu\n",
                static_cast<unsigned long long>(r.candidates_checked));
    std::printf("step2_masked_out   = %llu\n", static_cast<unsigned long long>(r.step2_masked_out));
    std::printf("fraction 2^%u       = %.10g  (%.6f%%)\n", N, fraction, fraction * 100.0);
    std::printf("step3_steps_total  = %llu\n",
                static_cast<unsigned long long>(r.step3_steps_total));
    std::printf("coverage_checksum  = %llu\n",
                static_cast<unsigned long long>(r.coverage_checksum));
    std::printf("pow_checksum       = %llu\n", static_cast<unsigned long long>(r.pow_checksum));
    std::printf("blocks_total       = %llu\n", static_cast<unsigned long long>(r.blocks_total));
    std::printf("max_excursion      = %s\n", collatz::verify::max_excursion_decimal(r).c_str());
    std::printf("max_excursion_n    = %s\n", collatz::to_string(r.max_excursion_n).c_str());
    std::printf("max_exceeds_u128   = %s\n", r.max_exceeds_u128 ? "yes" : "no");
}

// Prints max_exact in verify's wording: the record against the bound on the
// peaks of un-walked numbers.
void print_merged_max_exact(unsigned N,
                            collatz::u128 block_h,
                            const ckp::MergeInfo& mi,
                            const collatz::verify::Result& r) {
    const unsigned depth = mi.depth;
    const double bound_log2 = collatz::verify::unwalked_bound_log2(N, depth, block_h);
    const double rec_log2 = collatz::verify::max_excursion_log2(r);
    const char* tag = block_h != 0 ? "[block] " : "";
    if (mi.max_unknown) {
        std::printf("max_exact          = no   %s[WARNING: a slice's maximum passed 2^128 "
                    "without its exact value (GPU or older slice file), so the merged "
                    "max_excursion is only a LOWER BOUND]\n",
                    tag);
    } else if (block_h != 0) {
        if (r.max_exact) {
            std::printf("max_exact          = yes  [block] (record 2^%.2f > un-walked peak "
                        "bound 2^%.2f, depth K=%u: no never-walked prefix value within the "
                        "block reaches the record; the maximum below the block end is "
                        "max(G(L), record))\n",
                        rec_log2,
                        bound_log2,
                        depth);
        } else {
            std::printf("max_exact          = no   [block] [WARNING: record 2^%.2f <= un-walked "
                        "peak bound 2^%.2f (depth K=%u): a never-walked prefix within the "
                        "block could hold a higher value]\n",
                        rec_log2,
                        bound_log2,
                        depth);
        }
    } else if (r.max_exact && rec_log2 > bound_log2) {
        std::printf("max_exact          = yes  (record 2^%.2f > un-walked peak bound "
                    "2^%.2f; depth K=%u)\n",
                    rec_log2,
                    bound_log2,
                    depth);
    } else if (r.max_exact) {
        std::printf("max_exact          = yes  (preflight walks every n < 2^%u "
                    "directly)\n",
                    N);
    } else {
        std::printf("max_exact          = no   [WARNING: max_excursion is a LOWER BOUND by "
                    "the un-walked-peak argument -- record 2^%.2f <= bound 2^%.2f (depth "
                    "K=%u); a never-walked excluded trajectory could hold a higher peak]\n",
                    rec_log2,
                    bound_log2,
                    depth);
    }
}

void print_merged_records(const collatz::verify::Options& o, const collatz::verify::Result& r) {
    if (!o.record_threshold.empty()) {
        std::printf("record_threshold   = %s\n", o.record_threshold.c_str());
        std::string list = "[";
        for (std::size_t i = 0; i < r.records.size(); ++i) {
            list += (i == 0 ? "[" : ", [") + collatz::to_string(r.records[i].n) + ", " +
                    r.records[i].peak + "]";
        }
        std::printf("records            = %s]\n", list.c_str());
        if (r.records_truncated) {
            std::printf("records_truncated  = yes\n");
            std::fprintf(stderr,
                         "warning: more than %zu walked candidates exceed the record threshold; "
                         "`records` holds only the %zu smallest n (records_truncated = yes)\n",
                         collatz::verify::kRecordsCap,
                         collatz::verify::kRecordsCap);
        }
    }
}

} // namespace collatz::cli::cmd_merge
