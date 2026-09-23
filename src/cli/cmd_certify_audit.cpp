// The audit mode of `collatz certify` (--audit-seed): certify::audit on the
// run's `verify --json` result (cmd_certify_result.cpp) and the audit JSON; see
// cmd_certify.cpp.

#include "cmd_certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/worker.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

namespace collatz::cli::cmd_certify {
namespace {

std::string audit_json(const collatz::certify::Options& o,
                       const collatz::certify::AuditOptions& ao,
                       const collatz::certify::AuditReport& rep,
                       const std::string& digest) {
    namespace wk = collatz::worker;
    const auto u = [](std::uint64_t v) { return std::to_string(v); };
    std::string s = "{\n  \"audit\": \"collatz-certify-audit/1\",\n";
    s += "  \"block\": {\"h\": \"" + collatz::to_string(o.block_h) + "\", \"K\": " + u(o.N) +
         ", \"L\": \"" + collatz::to_string(rep.L) + "\"},\n";
    s += "  \"params\": {\"A\": " + u(o.A) + ", \"B\": " + u(o.B) +
         ", \"bitvectors\": " + u(o.num_bitvectors) + ", \"mod3t\": " + u(o.mod3t) +
         ", \"partial_oee\": " + (o.partial_oee ? "true" : "false") +
         ", \"window_chains\": " + u(o.window_chains) +
         ", \"window_templates_D\": " + u(o.window_templates_D) + "},\n";
    s += "  \"seed\": " + wk::json_quote(ao.seed) + ",\n";
    s += "  \"result_digest\": " + (digest.empty() ? std::string("null") : wk::json_quote(digest)) +
         ",\n";
    s += "  \"samples\": " + u(rep.samples) + ",\n";
    s += "  \"columns\": " + u(rep.columns) + ",\n";
    s += "  \"audited_classes\": " + u(rep.audited_classes) + ",\n";
    s += "  \"step1_classes\": " + u(rep.step1_classes) + ",\n";
    s += "  \"survivors\": " + u(rep.survivors) + ",\n";
    s += "  \"audited_numbers\": " + u(rep.audited_numbers) + ",\n";
    s += "  \"exclusions_checked\": " + u(rep.exclusions_checked) + ",\n";
    s += "  \"candidates_walked\": " + u(rep.candidates_walked) + ",\n";
    s += "  \"peaks_checked\": " + u(rep.peaks_checked) + ",\n";
    s += "  \"records_checked\": " + u(rep.records_checked) + ",\n";
    s += "  \"excursions_recomputed\": " + u(rep.excursions_recomputed) + ",\n";
    s += "  \"failures_total\": " + u(rep.failures_total) + ",\n";
    s += "  \"failures\": [";
    for (std::size_t i = 0; i < rep.failures.size(); ++i) {
        const collatz::certify::Failure& f = rep.failures[i];
        s += std::string(i == 0 ? "\n" : ",\n") + "    {\"n0\": \"" + f.n0 + "\", \"n\": \"" + f.n +
             "\", \"k\": " + u(f.k) + ", \"reason\": " + wk::json_quote(f.reason) + "}";
    }
    s += rep.failures.empty() ? "],\n" : "\n  ],\n";
    char secs[64];
    std::snprintf(secs, sizeof(secs), "%.3f", rep.seconds);
    s += "  \"seconds\": " + std::string(secs) + "\n}\n";
    return s;
}

} // namespace

/// Audit mode of run_certify: `o` and `ao` hold the parsed options.
int run_audit(collatz::certify::Options& o,
              collatz::certify::AuditOptions& ao,
              const std::string& result_path,
              const std::string& json_path) {
    if (o.block_h == 0) {
        std::fprintf(stderr, "error: --audit-seed audits a block: give --block H\n");
        return 2;
    }
    std::string digest;
    std::string err;
    if (!result_path.empty() && !load_run_result(result_path, o, ao, digest, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    // The audit reads a few thousand windows of the tables, so the read-ahead
    // of the whole mapping that verify wants would only cost I/O and memory.
    if (std::getenv("COLLATZ_MMAP_WILLNEED") == nullptr) {
        _putenv_s("COLLATZ_MMAP_WILLNEED", "0");
    }
    collatz::certify::AuditReport rep;
    try {
        rep = collatz::certify::audit(o, ao);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    const std::string doc = audit_json(o, ao, rep, digest);
    std::fputs(doc.c_str(), stdout);
    std::fflush(stdout);
    if (!json_path.empty()) {
        // write_file_atomic writes <path>.tmp and renames it; when the rename
        // fails (the path is a directory, say) it leaves the temp file, which
        // is removed here unless it was there before.
        namespace fs = std::filesystem;
        const fs::path tmp = fs::path(json_path + ".tmp");
        std::error_code ec;
        const bool had_tmp = fs::exists(fs::symlink_status(tmp, ec));
        if (!collatz::checkpoint::write_file_atomic(json_path, doc, err)) {
            if (!had_tmp) {
                fs::remove(tmp, ec);
            }
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }
    std::fprintf(stderr,
                 "certify audit: block %s K=%u: %llu residues, %llu classes (%llu survivors), "
                 "%llu exclusions re-derived, %llu candidates walked, %llu failures, %.1f s\n",
                 collatz::to_string(o.block_h).c_str(),
                 o.N,
                 static_cast<unsigned long long>(rep.samples),
                 static_cast<unsigned long long>(rep.audited_classes),
                 static_cast<unsigned long long>(rep.survivors),
                 static_cast<unsigned long long>(rep.exclusions_checked),
                 static_cast<unsigned long long>(rep.candidates_walked),
                 static_cast<unsigned long long>(rep.failures_total),
                 rep.seconds);
    return rep.clean() ? 0 : 1;
}

} // namespace collatz::cli::cmd_certify
