#include "cmd_verify_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cmath>
#include <cstdio>
#include <string>

// Prints the result of a verify run: the run configuration, the counts and
// digests, the exact-maximum verdict, the records and the timings.

namespace collatz::cli::verify_detail {

namespace {

double safe_log2(double x) {
    return x > 0.0 ? std::log2(x) : 0.0;
}

void print_run_config(const collatz::verify::Options& opts,
                      const VerifyArgs& va,
                      const char* backend) {
    std::printf("verify: N=%u A=%u B=%u bitvectors=%u threads=%u kernel=table16 backend=%s\n",
                opts.N,
                opts.A,
                opts.B,
                opts.num_bitvectors,
                opts.threads,
                backend);
    if (va.have_block) {
        const collatz::u128 L = collatz::verify::block_start(opts);
        const collatz::u128 top = L + (static_cast<collatz::u128>(1) << opts.N);
        std::printf("block: H=%s K=%u L=%s end=%s value_bits=%u  (checks [L, end), assuming "
                    "every n < L converges)\n",
                    collatz::to_string(opts.block_h).c_str(),
                    opts.N,
                    collatz::to_string(L).c_str(),
                    collatz::to_string(top).c_str(),
                    collatz::verify::value_bits(opts));
        std::printf("assumption_source  = %s\n", collatz::verify::assumption_source(opts).c_str());
    }
    std::printf("config: mod3t=%u partial_oee=%s window_chains=%u window_templates_D=%u\n",
                opts.step1.mod3t,
                opts.partial_oee ? "on" : "off",
                opts.window_chains,
                opts.window_templates_D);
}

void print_counts(const collatz::verify::Options& opts, const collatz::verify::Result& r) {
    const double total = std::ldexp(1.0, static_cast<int>(opts.N));
    const double fraction = static_cast<double>(r.candidates_checked) / total;
    std::printf("candidates_checked = %llu\n",
                static_cast<unsigned long long>(r.candidates_checked));
    std::printf("step2_masked_out   = %llu\n", static_cast<unsigned long long>(r.step2_masked_out));
    std::printf("fraction 2^%u       = %.10g  (%.6f%%)\n", opts.N, fraction, fraction * 100.0);
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

// `k113` is the un-walked depth K of the exact-maximum bound.
void print_max_exact(const collatz::verify::Options& opts,
                     const collatz::verify::Result& r,
                     unsigned k113) {
    const double bound_log2 = collatz::verify::unwalked_bound_log2(opts.N, k113, opts.block_h);
    const double rec_log2 = collatz::verify::max_excursion_log2(r);
    if (opts.block_h != 0) {
        // In block mode the bound covers the never-walked prefixes of the
        // block's numbers; it does not make the record the block maximum.
        if (r.max_exact) {
            std::printf("max_exact          = yes  [block] (record 2^%.2f > un-walked peak "
                        "bound 2^%.2f, depth K=%u: no never-walked prefix value within the "
                        "block reaches the record; the maximum below the block end is "
                        "max(G(L), record))\n",
                        rec_log2,
                        bound_log2,
                        k113);
        } else {
            std::printf("max_exact          = no   [block] [WARNING: record 2^%.2f <= un-walked "
                        "peak bound 2^%.2f (depth K=%u): a never-walked prefix within the "
                        "block could hold a higher value]\n",
                        rec_log2,
                        bound_log2,
                        k113);
        }
    } else if (r.max_exact) {
        if (rec_log2 > bound_log2) {
            std::printf("max_exact          = yes  (record 2^%.2f > un-walked peak bound "
                        "2^%.2f; depth K=%u)\n",
                        rec_log2,
                        bound_log2,
                        k113);
        } else {
            std::printf("max_exact          = yes  (preflight walks every n < 2^%u "
                        "directly)\n",
                        opts.N);
        }
    } else {
        std::printf("max_exact          = no   [WARNING: max_excursion is a LOWER BOUND by "
                    "the un-walked-peak argument -- record 2^%.2f <= bound 2^%.2f (depth "
                    "K=%u); a never-walked excluded trajectory could hold a higher peak]\n",
                    rec_log2,
                    bound_log2,
                    k113);
    }
}

void print_records(const collatz::verify::Options& opts, const collatz::verify::Result& r) {
    if (!opts.record_threshold.empty()) {
        std::printf("record_threshold   = %s\n", opts.record_threshold.c_str());
        std::string list = "[";
        for (std::size_t i = 0; i < r.records.size(); ++i) {
            list += (i == 0 ? "[" : ", [") + collatz::to_string(r.records[i].n) + ", " +
                    r.records[i].peak + "]";
        }
        list += ']';
        std::printf("records            = %s\n", list.c_str());
        if (r.records_truncated) {
            std::printf("records_truncated  = yes\n");
            std::fprintf(stderr,
                         "warning: more than %zu walked candidates exceed --record-threshold; "
                         "`records` holds only the %zu smallest n (records_truncated = yes). "
                         "Raise the threshold.\n",
                         opts.records_cap,
                         opts.records_cap);
        }
    }
}

void print_timing(const collatz::verify::Options& opts, const collatz::verify::Result& r) {
    const double total = std::ldexp(1.0, static_cast<int>(opts.N));
    const double nps = r.seconds_total > 0.0 ? total / r.seconds_total : 0.0;
    std::printf("seconds_step1_2    = %.6f\n", r.seconds_step1_2);
    std::printf("seconds_filter     = %.6f  (bv_cache=%s%s)\n",
                r.seconds_filter,
                r.bv_cache_hit ? "hit" : "miss",
                r.bv_cache_hit ? "" : " [cold build]");
    std::printf("seconds_total      = %.6f\n", r.seconds_total);
    std::printf("numbers/sec        = %.6g  (log2 = %.3f)\n", nps, safe_log2(nps));
}

} // namespace

void print_result(const collatz::verify::Options& opts,
                  const VerifyArgs& va,
                  const collatz::verify::Result& r,
                  const char* backend) {
    print_run_config(opts, va, backend);
    print_counts(opts, r);
    const unsigned k113 = collatz::verify::unwalked_depth(opts);
    print_max_exact(opts, r, k113);
    std::printf("overflow_fallbacks = %llu\n",
                static_cast<unsigned long long>(r.overflow_fallbacks));
    print_records(opts, r);
    std::printf("prefix_walks       = %llu\n", static_cast<unsigned long long>(r.prefix_walks));
    std::printf("step1_survivors    = %llu\n", static_cast<unsigned long long>(r.step1.survivors));
    std::printf("result_digest      = %s\n",
                collatz::checkpoint::result_digest(opts, k113, r).c_str());
    print_timing(opts, r);
}

} // namespace collatz::cli::verify_detail
