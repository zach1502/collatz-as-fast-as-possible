// The canonical result digest text, its SHA-256 (result_digest), and the check
// that a slice file's recorded digest matches its fields.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/sha256.hpp"

#include <algorithm>

namespace collatz::checkpoint {

std::string
result_digest_text(const verify::Options& opts, unsigned unwalked_depth, const verify::Result& r) {
    std::string s = std::string(kResultDigestSchema) + "\n";
    const auto line = [&s](const char* key, const std::string& value) {
        s += key;
        s += '=';
        s += value;
        s += '\n';
    };
    const auto num = [](std::uint64_t v) { return std::to_string(v); };
    line("block_h", collatz::to_string(opts.block_h));
    line("K", num(opts.N));
    line("assumes_below", verify::assumes_below(opts));
    line("case", num(opts.step1.case_c) + "/" + num(opts.step1.case_k));
    line("A", num(opts.A));
    line("B", num(opts.B));
    line("bitvectors", num(opts.num_bitvectors));
    line("kernel", "table16");
    line("mod3t", num(opts.step1.mod3t));
    line("partial_oee", opts.partial_oee ? "1" : "0");
    line("window_chains", num(opts.window_chains));
    line("window_templates_D", num(opts.window_templates_D));
    line("unwalked_depth", num(unwalked_depth));
    line("record_threshold", opts.record_threshold.empty() ? "-" : opts.record_threshold);
    line("candidates_checked", num(r.candidates_checked));
    line("step2_masked_out", num(r.step2_masked_out));
    line("step1_survivors", num(r.step1.survivors));
    line("step3_steps_total", num(r.step3_steps_total));
    line("blocks_total", num(r.blocks_total));
    line("coverage_checksum", num(r.coverage_checksum));
    line("pow_checksum", num(r.pow_checksum));
    line("pow_kernel_id", num(r.pow_kernel_id));
    line("max_excursion", verify::max_excursion_decimal(r));
    line("max_excursion_n", collatz::to_string(r.max_excursion_n));
    line("max_exceeds_u128", r.max_exceeds_u128 ? "1" : "0");
    line("max_exact", r.max_exact ? "1" : "0");
    line("overflow_fallbacks", num(r.overflow_fallbacks));
    line("preflight",
         num(r.preflight_candidates) + "," + num(r.preflight_coverage) + "," +
             num(r.preflight_pow) + "," + num(r.preflight_blocks) + "," +
             num(r.preflight_step3_steps));
    std::string recs;
    for (const verify::RecordHit& h : r.records) {
        if (!recs.empty()) {
            recs += ';';
        }
        recs += collatz::to_string(h.n) + ":" + h.peak;
    }
    line("records", recs);
    line("records_truncated", r.records_truncated ? "1" : "0");
    return s;
}

std::string
result_digest(const verify::Options& opts, unsigned unwalked_depth, const verify::Result& r) {
    return collatz::sha256_hex(result_digest_text(opts, unwalked_depth, r));
}

namespace detail {

bool check_slice_digest(const Json* res, const Slice& s, std::string& err) {
    const Json* schema = res->get("result_digest_schema");
    if (schema == nullptr || schema->type != Json::Type::String ||
        schema->text != kResultDigestSchema) {
        err = std::string("result.result_digest_schema is not ") + kResultDigestSchema +
              " (a file of another version of this program?)";
        return false;
    }
    const Json* d = res->get("result_digest");
    const bool hex = d != nullptr && d->type == Json::Type::String && d->text.size() == 64 &&
                     std::all_of(d->text.begin(), d->text.end(), [](char c) {
                         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                     });
    if (!hex) {
        err = "result.result_digest is missing or not a lowercase SHA-256 hex digest";
        return false;
    }
    const std::string want = result_digest(s.opts, s.unwalked_depth, s.r);
    if (d->text != want) {
        err = "result.result_digest " + d->text.substr(0, 16) +
              "... does not match the digest of the file's fields (" + want.substr(0, 16) +
              "...): the file was changed after it was written";
        return false;
    }
    return true;
}

} // namespace detail

} // namespace collatz::checkpoint
