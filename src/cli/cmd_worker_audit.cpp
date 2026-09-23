// Audits of accepted units: the queue, the low-priority `collatz certify
// --audit-seed` child and the thread that waits for it. The report to POST
// /api/audit is in cmd_worker_audit_report.cpp.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace collatz::cli::worker_detail {

void Worker::queue_audit(const wk::Lease& l, const fs::path& udir, const std::string& digest) {
    if (digest.empty() || !wk::audit_selected(digest, s_.audit_rate)) {
        return;
    }
    finish_audit(false); // an audit that ended frees the slot
    const std::string unit = "unit " + std::to_string(l.unit_id);
    if (audit_) {
        log_line("the audit of " + unit + " was skipped: the audit of unit " +
                 std::to_string(audit_->unit_id) + " has not finished");
        return;
    }
    AuditJob job;
    job.unit_id = l.unit_id;
    job.seed = digest;
    job.dir = state_dir_ / "audits" / std::to_string(l.unit_id);
    std::error_code ec;
    fs::remove_all(job.dir, ec);
    fs::create_directories(job.dir, ec);
    if (!ec) {
        fs::copy_file(udir / "result.json", job.dir / "result.json", ec);
    }
    if (ec) {
        std::fprintf(stderr,
                     "worker: cannot prepare the audit of %s in %s (%s)\n",
                     unit.c_str(),
                     job.dir.string().c_str(),
                     ec.message().c_str());
        log_line("the audit of " + unit + " was skipped: " + ec.message());
        return;
    }
    job.argv = {wk::self_executable()};
    const std::vector<std::string> args = wk::audit_args(
        l, digest, 1, (job.dir / "result.json").string(), (job.dir / "audit.json").string());
    job.argv.insert(job.argv.end(), args.begin(), args.end());
    log_line(unit + " was picked for an audit (seed " + digest.substr(0, 16) + "...)");
    audit_ = std::move(job);
}

void Worker::start_audit() {
    if (!audit_ || audit_run_) {
        return;
    }
    const auto run = std::make_shared<AuditRun>();
    wk::ProcessOptions po;
    po.stdout_path = (audit_->dir / "audit.log").string();
    po.stderr_path = po.stdout_path;
    po.low_priority = true;
    po.started = [run](long long) { run->phase.store(1); };
    // stop_audit asks for it; a quit (a second Ctrl-C or SIGTERM) too.
    po.cancel = [run] { return run->stop.load() || g_stop.load() >= 2; };
    const std::string unit = "unit " + std::to_string(audit_->unit_id);
    const fs::path log = display_on_ ? log_path_ : fs::path();
    g_audit_running.store(true);
    try {
        audit_thread_ = std::thread([run, argv = audit_->argv, po, unit, log] {
            std::string err;
            const int rc = wk::run_process(argv, po, err);
            run->rc = rc;
            run->err = err;
            g_audit_running.store(false);
            // A quit while no verify runs: worker_on_signal left the exit to
            // this thread, so that the audit process is gone first. (A quit
            // during a verify is handled by run() once that verify stops.)
            if (g_stop.load() >= 2 && !g_child_running.load()) {
                append_log(log, "quit: the audit of " + unit + " was stopped (process terminated)");
                wk::restore_terminal();
                std::_Exit(kExitAborted);
            }
            run->phase.store(2);
        });
    } catch (const std::system_error& e) {
        g_audit_running.store(false);
        log_line("the audit of unit " + std::to_string(audit_->unit_id) +
                 " was not run: no thread (" + e.what() + ")");
        std::error_code ec;
        fs::remove_all(audit_->dir, ec);
        audit_.reset();
        return;
    }
    // Return only once the child exists, so that no other child is created
    // while this one inherits handles (Windows).
    while (run->phase.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    audit_run_ = run;
    log_line("auditing unit " + std::to_string(audit_->unit_id) +
             " in the background (1 thread, low priority)");
}

// audit_run_ is set only while audit_ holds its job: start_audit runs only with
// audit_ set, and finish_audit and stop_audit reset both.
// NOLINTBEGIN(bugprone-unchecked-optional-access)
void Worker::finish_audit(bool wait) {
    if (!audit_run_) {
        return;
    }
    if (audit_run_->phase.load() != 2) {
        if (!wait) {
            return;
        }
        std::printf("worker: waiting for the audit of unit %llu to finish (Ctrl-C stops it)\n",
                    static_cast<unsigned long long>(audit_->unit_id));
        std::fflush(stdout);
        status_ = "waiting for the audit of unit " + std::to_string(audit_->unit_id);
        while (audit_run_->phase.load() != 2) {
            if (stop_requested()) {
                hide_display();
                stop_audit("the worker was asked to pause");
                return;
            }
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        hide_display();
    }
    audit_thread_.join();
    const std::shared_ptr<AuditRun> run = std::move(audit_run_);
    audit_run_.reset();
    const AuditJob job = std::move(*audit_);
    audit_.reset();
    report_audit(job, *run);
}
// NOLINTEND(bugprone-unchecked-optional-access)

void Worker::stop_audit(const std::string& why) {
    if (!audit_) {
        return;
    }
    const std::string unit = "unit " + std::to_string(audit_->unit_id);
    std::error_code ec;
    if (!audit_run_) {
        log_line("the audit of " + unit + " was not run: " + why);
        fs::remove_all(audit_->dir, ec);
        audit_.reset();
        return;
    }
    if (audit_run_->phase.load() != 2) {
        // The waiting thread terminates the process within about 200 ms.
        audit_run_->stop.store(true);
        while (audit_run_->phase.load() != 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    audit_thread_.join();
    const std::shared_ptr<AuditRun> run = std::move(audit_run_);
    audit_run_.reset();
    const AuditJob job = std::move(*audit_);
    audit_.reset();
    if (run->rc != wk::kTerminatedExit) {
        report_audit(job, *run); // it ended on its own first
        return;
    }
    std::printf("worker: stopped the audit of %s (%s)\n", unit.c_str(), why.c_str());
    std::fflush(stdout);
    log_line("the audit of " + unit + " was stopped (" + why + "): its process was terminated");
    fs::remove_all(job.dir, ec);
}

void Worker::remove_stale_audits() {
    const fs::path root = state_dir_ / "audits";
    const std::size_t removed = wk::remove_stale_audit_dirs(root);
    if (removed > 0) {
        log_line("removed " + std::to_string(removed) + " audit director" +
                 (removed == 1 ? "y" : "ies") + " left by an earlier session in " + root.string());
    }
}

} // namespace collatz::cli::worker_detail
