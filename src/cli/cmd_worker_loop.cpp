// The worker's session: the signal handler, the host id in the config,
// Worker::run and the fetch-work-submit loop Worker::run_units.

#include "cmd_worker_internal.hpp"
#include "collatz/sha256.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace collatz::cli::worker_detail {

namespace {

extern "C" void worker_on_signal(int sig) {
    if (g_stop.fetch_add(1) + 1 >= 2 && !g_child_running.load() && !g_audit_running.load()) {
        // The CRT runs this handler on a thread of its own, where the console
        // calls are allowed.
        wk::restore_terminal();
        std::_Exit(kExitAborted);
    }
    std::signal(sig, worker_on_signal); // MSVCRT/UCRT reset the handler on delivery
}

} // namespace

bool Worker::load_identity(std::string& err) {
    if (!wk::load_config(s_.config_path, cfg_, err)) {
        return false;
    }
    if (cfg_.host_id.empty()) {
        cfg_.host_id = wk::random_hex(16);
        if (cfg_.host_id.empty()) {
            err = "the OS random number source failed; no host id was created";
            return false;
        }
    }
    cfg_.server = s_.server;
    return wk::save_config(s_.config_path, cfg_, err);
}

int Worker::run() {
    std::string err;
    // The state directory first: nothing below is deleted or changed before
    // it is known to be the worker's own.
    if (!wk::make_private_dirs(dir_, err) || !wk::open_state_dir(state_dir_, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return kExitError;
    }
    // Lock first: two first starts on one config would otherwise each save a
    // different new host id.
    const fs::path lock = state_dir_ / "worker.lock";
    if (!wk::acquire_lock(lock, err)) {
        std::fprintf(stderr,
                     "error: another worker uses this config (%s); give a second worker its own "
                     "--config FILE in another directory\n",
                     err.c_str());
        return kExitError;
    }
    struct LockGuard {
        fs::path p;
        ~LockGuard() {
            wk::release_lock(p);
        }
    } lock_guard{lock};
    if (!load_identity(err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return kExitError;
    }
    // Request files of a killed earlier session hold the worker token; remove them.
    (void) wk::remove_stale_request_files(tmp_dir_);
    // A PAUSE left by an earlier session is a stale request. One that cannot
    // be removed (a non-empty directory, a read-only state directory) would
    // pause every start, so the worker stops with the reason instead.
    std::error_code ec;
    if (fs::exists(fs::symlink_status(pause_file_, ec))) {
        std::error_code rm;
        fs::remove(pause_file_, rm);
        if (rm || fs::exists(fs::symlink_status(pause_file_, ec))) {
            std::fprintf(stderr,
                         "error: cannot remove %s, the pause request of an earlier session (%s); "
                         "remove it, then run the same command\n",
                         pause_file_.string().c_str(),
                         rm ? rm.message().c_str() : "it is still there");
            return kExitError;
        }
    }
    // One directory per server, so units of two servers never mix. The name
    // stays fixed: the next run finds a pending unit by it.
    units_dir_ = state_dir_ / "units" / collatz::sha256_hex(s_.server + "\nowner").substr(0, 16);
    std::printf("collatz worker on %s (host %.8s, binary %.12s)\n"
                "  blocks of up to 2^%u numbers (--block-bits) on the %s%s\n"
                "  config %s; Ctrl-C or creating %s pauses after saving progress\n",
                s_.server.c_str(),
                cfg_.host_id.c_str(),
                COLLATZ_GIT_COMMIT,
                s_.block_bits,
                s_.gpu ? "GPU" : "CPU",
                s_.gpu && s_.recheck_cpu ? " (re-checks of GPU results also on the CPU)" : "",
                s_.config_path.string().c_str(),
                pause_file_.string().c_str());
    std::fflush(stdout);
    std::signal(SIGINT, worker_on_signal);
    std::signal(SIGTERM, worker_on_signal);
    start_display();
    log_line("start: " + s_.server + ", blocks of up to 2^" + std::to_string(s_.block_bits) +
             " numbers on the " + (s_.gpu ? "GPU" : "CPU") + " (binary " +
             std::string(COLLATZ_GIT_COMMIT).substr(0, 12) + ")");
    remove_stale_audits();
    resend_audit_reports();
    const int rc = run_units();
    // A normal end runs the queued audit and waits for it; any other stop
    // (a pause, a quit, an error) does not wait for an audit.
    if (rc == kExitDone && !stop_requested()) {
        start_audit();
        finish_audit(true);
    } else {
        stop_audit(rc == kExitAborted || g_stop.load() >= 2 ? "the worker quit"
                   : rc == kExitPaused || stop_requested()  ? "the worker paused"
                                                            : "the worker stopped on an error");
    }
    finish_display(rc);
    return rc;
}

int Worker::run_units() {
    std::uint64_t done = 0;
    unsigned failures = 0;
    unsigned fetch_failures = 0; // failed tries in a row to get a lease
    bool waiting_noted = false;
    for (;;) {
        finish_audit(false);
        if (stop_requested()) {
            std::printf("worker: paused; run the same command to continue\n");
            return kExitPaused;
        }
        wk::Lease lease;
        fs::path udir;
        std::optional<std::uint64_t> prefer;
        bool have = false;
        if (const std::optional<fs::path> p = pending_unit()) {
            bool released = false;
            std::string why;
            if (std::optional<wk::Lease> l = load_lease(*p, released);
                l && !lease_runnable(*l, why)) {
                std::fprintf(stderr,
                             "worker: dropped the saved unit %llu, which verify cannot run (%s)\n",
                             static_cast<unsigned long long>(l->unit_id),
                             why.c_str());
                drop(*p);
            } else if (l) {
                if (l->K > s_.block_bits) {
                    // Saved by a session with a larger --block-bits: hand it
                    // back (if still held) instead of running it.
                    if (!released) {
                        release(*l, *p);
                    }
                    std::printf("worker: handed back unit %llu, a block of 2^%u numbers (more "
                                "than --block-bits %u); asking for a smaller one\n",
                                static_cast<unsigned long long>(l->unit_id),
                                l->K,
                                s_.block_bits);
                    std::fflush(stdout);
                    drop(*p);
                } else if (!released) {
                    lease = *l;
                    udir = *p;
                    have = true;
                } else {
                    prefer = l->unit_id;
                }
            } else {
                drop(*p);
            }
        }
        if (!have) {
            const Got g = fetch(lease, udir, prefer);
            if (g == Got::Fatal) {
                return kExitError;
            }
            if (g == Got::Wait) {
                if (no_work_ && s_.max_units == 1) {
                    std::printf("worker: no work available right now (%s)\n", wait_reason_.c_str());
                    return kExitDone;
                }
                // --once is a bounded run: a server or curl that keeps failing
                // ends it instead of keeping it waiting.
                if (!no_work_ && s_.max_units == 1 && ++fetch_failures >= kOnceTries) {
                    std::fprintf(stderr,
                                 "error: no lease after %u tries (%s); stopping (--once)\n",
                                 kOnceTries,
                                 wait_reason_.c_str());
                    return kExitError;
                }
                if (!waiting_noted) {
                    std::printf("worker: no lease (%s); retrying in %u s\n",
                                wait_reason_.c_str(),
                                backoff_ / 1000);
                    std::fflush(stdout);
                    waiting_noted = true;
                }
                start_audit(); // the machine is idle until the next lease
                (void) nap(backoff_,
                           (no_work_ ? "no work right now: " : "no lease: ") + wait_reason_);
                continue;
            }
            waiting_noted = false;
            fetch_failures = 0;
            if (prefer && lease.unit_id != *prefer) {
                drop(units_dir_ / std::to_string(*prefer)); // handed to someone else meanwhile
            }
        }
        const Unit u = work(lease, udir);
        switch (u) {
        case Unit::Done:
            failures = 0;
            if (s_.max_units != 0 && ++done >= s_.max_units) {
                return kExitDone;
            }
            break;
        case Unit::Rejected:
        case Unit::Failed:
            if (++failures >= 3) {
                std::fprintf(stderr, "error: 3 units in a row failed; stopping\n");
                return kExitError;
            }
            (void) nap(next_backoff(), "the last unit failed (see above); trying again");
            break;
        case Unit::Renew:
            // The next pass asks for the unit with prefer_unit; counted so that a
            // server that keeps refusing the lease cannot loop the worker.
            if (++failures >= 3) {
                std::fprintf(stderr, "error: 3 units in a row failed; stopping\n");
                return kExitError;
            }
            break;
        case Unit::Paused:
            std::printf("worker: paused; the unit's progress is saved. Run the same command "
                        "to continue\n");
            return kExitPaused;
        case Unit::Aborted:
            std::printf("worker: aborted; the lease is kept. Run the same command to resume "
                        "the unit\n");
            return kExitAborted;
        case Unit::TokenRefused:
            std::fprintf(stderr,
                         "error: the unit's result is kept; check the worker token (--token-file "
                         "or COLLATZ_WORKER_TOKEN) and run the same command to submit it\n");
            return kExitError;
        case Unit::Unsent:
            return kExitError; // submit() said why; the result stays for the next run
        }
    }
}

} // namespace collatz::cli::worker_detail
