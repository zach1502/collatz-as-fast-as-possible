// The outcome of an audit: printed, logged and posted to /api/audit
// (report_audit). An outcome the server could not take is kept in the audit's
// directory as wk::kUnreportedAudit and sent again at the next start
// (resend_audit_reports); the server answers a repeated audit as a duplicate.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/worker.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace collatz::cli::worker_detail {

namespace {

// An audit's figures as /api/audit takes them, apart from the token and host.
struct Outcome {
    std::uint64_t unit_id = 0;
    std::string seed;
    std::uint64_t samples = 0;
    std::uint64_t failures = 0;
    std::string seconds; // a JSON number
};

std::string outcome_fields(const Outcome& o) {
    return "\"unit_id\":" + std::to_string(o.unit_id) + ",\"seed\":" + wk::json_quote(o.seed) +
           ",\"samples\":" + std::to_string(o.samples) +
           ",\"failures\":" + std::to_string(o.failures) + ",\"seconds\":" + o.seconds;
}

// Whether the server took the outcome or refused it for good. No answer, a
// refused token, a timeout, a rate limit or a server error is worth another
// try at the next start.
bool settled(const Reply& r) {
    if (r.status >= 200 && r.status < 300) {
        return true;
    }
    return r.status >= 400 && r.status < 500 && r.status != 401 && r.status != 403 &&
           r.status != 408 && r.status != 429;
}

std::string failure_text(const Reply& r) {
    return r.json && !r.error.empty() ? "HTTP " + std::to_string(r.status) + ": " + r.error
                                      : reply_error(r);
}

} // namespace

void Worker::report_audit(const AuditJob& job, const AuditRun& run) {
    const std::string unit = "unit " + std::to_string(job.unit_id);
    std::string text;
    std::optional<Json> doc;
    // Exit 0 is a clean audit, 1 one with failures; both write audit.json.
    if ((run.rc == 0 || run.rc == 1) && read_file(job.dir / "audit.json", text)) {
        doc = collatz::checkpoint::parse_json(text);
    }
    std::uint64_t samples = 0;
    std::uint64_t failures = 0;
    if (!doc || !parse_u64(num_of(&*doc, "samples"), samples) ||
        !parse_u64(num_of(&*doc, "failures_total"), failures)) {
        std::error_code ec;
        if (g_stop.load() > 0) {
            // Ctrl-C reaches the audit child too.
            log_line("the audit of " + unit + " was interrupted");
            fs::remove_all(job.dir, ec);
            return;
        }
        std::string log;
        (void) read_file(job.dir / "audit.log", log);
        // Only a start failure sets err: a Windows NTSTATUS exit code is negative.
        const std::string why =
            !run.err.empty() ? run.err : "exit code " + wk::exit_code_text(run.rc);
        std::fprintf(stderr,
                     "worker: the audit of %s did not finish (%s)\n%s\n",
                     unit.c_str(),
                     why.c_str(),
                     wk::redact(tail_lines(log, 6)).c_str());
        log_line("the audit of " + unit + " did not finish (" + why + "); see " + job.dir.string());
        return;
    }
    const double seconds = std::strtod(num_of(&*doc, "seconds").c_str(), nullptr);
    char line[200];
    std::snprintf(line,
                  sizeof(line),
                  "audit of %s (%llu residues, seed %.16s...): %llu failures, %.1f s",
                  unit.c_str(),
                  static_cast<unsigned long long>(samples),
                  job.seed.c_str(),
                  static_cast<unsigned long long>(failures),
                  seconds);
    if (failures == 0) {
        std::printf("%s\n", line);
        log_line(line);
    } else {
        // A failure means a soundness bug in the shared host code or bad
        // hardware, so the files are kept for inspection.
        const std::string where = (job.dir / "audit.json").string();
        std::fprintf(stderr, "worker: %s; see %s\n", line, where.c_str());
        log_line(std::string(line) + "; see " + where);
    }
    std::fflush(stdout);
    char secs[32];
    std::snprintf(secs, sizeof(secs), "%.3f", seconds);
    const Outcome o{job.unit_id, job.seed, samples, failures, secs};
    const Reply r = call("POST", "/api/audit", "{" + auth_fields() + "," + outcome_fields(o) + "}");
    if (r.status >= 200 && r.status < 300) {
        log_line("the audit of " + unit + " was reported to the server");
    } else if (settled(r)) {
        log_line("the server refused the audit of " + unit + " (" + failure_text(r) + ")");
    } else {
        std::string err;
        const bool kept = collatz::checkpoint::write_file_atomic(
            (job.dir / wk::kUnreportedAudit).string(), "{" + outcome_fields(o) + "}\n", err);
        log_line("could not report the audit of " + unit + " (" + failure_text(r) + ")" +
                 (kept ? "; it is sent again at the next start" : "; nor keep it: " + err));
        if (kept) {
            return; // the directory holds the outcome until the server takes it
        }
    }
    if (failures == 0) {
        std::error_code ec;
        fs::remove_all(job.dir, ec);
    }
}

void Worker::resend_audit_reports() {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(state_dir_ / "audits", ec)) {
        std::error_code e2;
        if (e.is_directory(e2) && fs::exists(e.path() / wk::kUnreportedAudit, e2)) {
            dirs.push_back(e.path());
        }
    }
    for (const fs::path& d : dirs) {
        const fs::path file = d / wk::kUnreportedAudit;
        std::string text;
        std::optional<Json> doc;
        if (read_file(file, text)) {
            doc = collatz::checkpoint::parse_json(text);
        }
        Outcome o;
        std::uint64_t id = 0;
        bool whole = doc && parse_u64(d.filename().string(), id) &&
                     parse_u64(num_of(&*doc, "unit_id"), o.unit_id) && o.unit_id == id &&
                     parse_u64(num_of(&*doc, "samples"), o.samples) &&
                     parse_u64(num_of(&*doc, "failures"), o.failures);
        if (doc && whole) {
            o.seed = wk::string_member(&*doc, "seed");
            o.seconds = num_of(&*doc, "seconds");
            whole = !o.seed.empty() && !o.seconds.empty();
        }
        std::error_code e2;
        if (!whole) {
            fs::remove(file, e2); // not an outcome this worker wrote
            continue;
        }
        const std::string unit = "unit " + std::to_string(id);
        const Reply r =
            call("POST", "/api/audit", "{" + auth_fields() + "," + outcome_fields(o) + "}");
        if (!settled(r)) {
            log_line("could not report the audit of " + unit + " of an earlier session (" +
                     failure_text(r) + "); it is sent again at the next start");
            continue;
        }
        log_line(r.status < 300
                     ? "the audit of " + unit + " was reported to the server"
                     : "the server refused the audit of " + unit + " (" + failure_text(r) + ")");
        fs::remove(file, e2);
        if (o.failures == 0) {
            fs::remove_all(d, e2);
        }
    }
}

} // namespace collatz::cli::worker_detail
