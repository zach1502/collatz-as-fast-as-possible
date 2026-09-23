// The block run's `verify --json` file that `collatz certify --audit-seed S
// --result FILE` audits: its digest, its parameters and the figures the audit
// checks; see cmd_certify.cpp.

#include "cmd_certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/sha256.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz/worker.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace collatz::cli::cmd_certify {

namespace cf = collatz::certify;
namespace ckp = collatz::checkpoint;
namespace wk = collatz::worker;

/// Reads the block run's `verify --json` file into the audit's reported values
/// and its figures (AuditOptions::run), and sets `digest` to the result digest
/// recomputed from its fields. False with a reason when the file does not
/// parse, lacks a field or holds an empty or non-canonical number, was changed
/// after verify wrote it (result.result_digest is not the digest of its
/// fields), is a slice, or ran with other parameters than `o`.
bool load_run_result(const std::string& path,
                     const cf::Options& o,
                     cf::AuditOptions& ao,
                     std::string& digest,
                     std::string& err) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in) {
        err = "cannot read " + path;
        return false;
    }
    const std::string text = ss.str();
    std::string why;
    const std::optional<ckp::Json> doc = ckp::parse_json(text, &why);
    if (!doc) {
        err = path + " is not JSON (" + why + ")";
        return false;
    }
    // The digest the work server computes, over the fields as written.
    // digest_text_from_json refuses a missing field and an empty or
    // non-canonical number, so "" cannot stand for an absent maximum or
    // threshold.
    std::string canon;
    if (!wk::digest_text_from_json(*doc, canon, why)) {
        err = path + ": " + why;
        return false;
    }
    const std::string computed = collatz::sha256_hex(canon);
    const std::string recorded = wk::string_member(doc->get("result"), "result_digest");
    if (recorded.empty()) {
        err = path + ": result.result_digest is missing or not a string";
        return false;
    }
    if (recorded != computed) {
        err = path + ": result.result_digest " + wk::printable(recorded, 24) +
              " is not the digest of the file's fields (" + computed.substr(0, 16) +
              "...): the file was changed after verify wrote it";
        return false;
    }
    // The same fields as verify's options and result. parse_slice checks the
    // digest on these values too, and the kernel and the value ranges.
    ckp::Slice s;
    if (!ckp::parse_slice(text, s, why)) {
        err = path + ": " + why;
        return false;
    }
    if (s.k != 1) {
        err = path + " is a slice (params.case_k != 1); the audit needs the whole block's result";
        return false;
    }
    if (s.opts.block_h != o.block_h) {
        err = path + " is block " + collatz::to_string(s.opts.block_h) + ", not --block " +
              collatz::to_string(o.block_h);
        return false;
    }
    collatz::verify::Options vo;
    vo.N = o.N;
    vo.A = o.A;
    vo.B = o.B;
    const struct {
        const char* key;
        unsigned have;
        unsigned want;
    } same[] = {{"N", s.opts.N, o.N},
                {"A", s.opts.A, o.A},
                {"B", s.opts.B, o.B},
                {"bitvectors", s.opts.num_bitvectors, o.num_bitvectors},
                {"mod3t", s.opts.step1.mod3t, o.mod3t},
                {"window_chains", s.opts.window_chains, o.window_chains},
                {"window_templates_D", s.opts.window_templates_D, o.window_templates_D},
                {"unwalked_depth", s.unwalked_depth, collatz::verify::unwalked_depth(vo)}};
    for (const auto& f : same) {
        if (f.have != f.want) {
            err = path + ": params." + f.key + " is " + std::to_string(f.have) + ", not " +
                  std::to_string(f.want) + ", the value this audit runs with";
            return false;
        }
    }
    if (s.opts.partial_oee != o.partial_oee) {
        err = path + ": params.partial_oee differs from this audit's";
        return false;
    }
    const collatz::verify::Result& r = s.r;
    ao.max_excursion = collatz::verify::max_excursion_decimal(r);
    ao.record_threshold = s.opts.record_threshold;
    ao.records.clear();
    for (const collatz::verify::RecordHit& h : r.records) {
        ao.records.push_back(cf::ReportedRecord{h.n, h.peak});
    }
    ao.records_truncated = r.records_truncated;
    ao.run = cf::ReportedRun{r.max_excursion_n,
                             r.candidates_checked,
                             r.step2_masked_out,
                             r.step1.survivors,
                             r.max_exact,
                             r.max_exceeds_u128};
    digest = computed;
    return true;
}

} // namespace collatz::cli::cmd_certify
