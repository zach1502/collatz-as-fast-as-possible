// One unit: `collatz verify --block` in a child process, after any verify
// that an earlier session left running on the same directory.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace collatz::cli::worker_detail {

bool Worker::wait_for_earlier_verify(const fs::path& udir) {
    const fs::path pid_file = udir / "verify.pid";
    std::string text;
    if (!read_file(pid_file, text)) {
        return true;
    }
    const long long pid = std::strtoll(text.c_str(), nullptr, 10);
    if (!wk::process_alive(pid)) {
        return true;
    }
    std::printf("worker: verify (pid %lld) from an earlier session still runs in %s; waiting for "
                "it to finish (delete %s if that process is not verify)\n",
                pid,
                udir.string().c_str(),
                pid_file.string().c_str());
    std::fflush(stdout);
    status_ = "waiting for verify (pid " + std::to_string(pid) + ") from an earlier session";
    std::error_code ec;
    bool go_on = true;
    while (wk::process_alive(pid) && fs::exists(pid_file, ec)) {
        if (stop_requested()) {
            // That verify also stops after its current slice once DIR/PAUSE exists.
            std::ofstream(udir / "ck" / "PAUSE").put('\n');
            go_on = false;
            break;
        }
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    hide_display();
    return go_on && !stop_requested();
}

Worker::Unit Worker::work(const wk::Lease& l, const fs::path& udir) {
    const fs::path ck = udir / "ck";
    const fs::path result = udir / "result.json";
    wk::keep_unit_files(udir, l); // slices and a result of another lease are not used
    // A finished result from a previous session (killed before it submitted).
    const auto finished = [&result] {
        std::string text;
        if (!read_file(result, text)) {
            return false;
        }
        const std::optional<Json> doc = collatz::checkpoint::parse_json(text);
        std::string dt;
        std::string err;
        return doc && wk::digest_text_from_json(*doc, dt, err);
    };
    if (finished()) {
        // Verified by an earlier session, unless it is this session's unit
        // coming back after a renewed lease.
        if (pending_unit_ != l.unit_id) {
            pending_numbers_ = 0.0;
        }
        return submit(l, udir, 0.0);
    }
    const bool go_on = wait_for_earlier_verify(udir);
    if (finished()) {
        if (pending_unit_ != l.unit_id) {
            pending_numbers_ = 0.0;
        }
        return submit(l, udir, 0.0);
    }
    if (!go_on) {
        release(l, udir);
        return Unit::Paused;
    }
    const fs::path pid_file = udir / "verify.pid";
    const fs::path progress_file = udir / "progress.txt";
    const auto t0 = std::chrono::steady_clock::now();
    const auto seconds = [&t0] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    std::vector<std::string> argv = {wk::self_executable()};
    // The server names the backend of each unit (a re-check of a GPU result
    // may ask for the CPU); it never turns on the GPU for a CPU worker.
    const bool gpu = wk::lease_uses_gpu(l, s_.gpu);
    const std::vector<std::string> args =
        wk::verify_args(l, s_.threads, gpu, ck.string(), result.string(), progress_file.string());
    argv.insert(argv.end(), args.begin(), args.end());
    wk::ProcessOptions po;
    po.stdout_path = (udir / "verify.log").string();
    po.stderr_path = po.stdout_path;
    {
        // A file of an earlier session would show its numbers until verify
        // writes its own.
        std::error_code ec;
        fs::remove(progress_file, ec);
    }
    unit_ = UnitView{};
    unit_.active = true;
    unit_.id = l.unit_id;
    unit_.h = l.h;
    unit_.K = l.K;
    unit_.gpu = gpu;
    unit_.t0 = t0;
    unit_.started = wk::local_time_text(std::time(nullptr), false);
    unit_.progress_file = progress_file;
    unit_.log_file = po.stdout_path;
    unit_.next_plain = t0 + std::chrono::minutes(1);
    po.poll = [&] {
        if (stop_requested()) {
            // verify --checkpoint stops after the current slice when DIR/PAUSE
            // exists. Recreate it in case verify removed it as stale at start.
            std::error_code ec;
            if (fs::is_directory(ck, ec) && !fs::exists(ck / "PAUSE", ec)) {
                std::ofstream(ck / "PAUSE").put('\n');
            }
        }
        tick();
    };
    po.started = [this, &pid_file](long long pid) {
        std::string werr;
        if (!collatz::checkpoint::write_file_atomic(
                pid_file.string(), std::to_string(pid) + "\n", werr)) {
            std::fprintf(stderr, "worker: %s\n", werr.c_str());
        }
        start_audit();
    };
    std::string err;
    g_child_running.store(true);
    const int rc = wk::run_process(argv, po, err);
    g_child_running.store(false);
    {
        std::error_code ec;
        fs::remove(pid_file, ec);
    }
    // verify writes its final counts as it exits; the slices it found on disk
    // are not this session's numbers.
    refresh_unit();
    const double covered = unit_.have
                               ? wk::block_progress(unit_.snap, l.K, seconds()).session_numbers
                               : std::ldexp(1.0, static_cast<int>(l.K));
    unit_.active = false;
    hide_display();
    if (rc == 0) {
        pending_numbers_ = covered;
        pending_unit_ = l.unit_id;
        return submit(l, udir, seconds());
    }
    if (rc == kExitPaused || rc == kExitAborted || g_stop.load() > 0) {
        // The finished slices are kept for the next start; they are this
        // session's work all the same.
        session_numbers_ += unit_.have ? covered : 0.0;
    }
    if (rc == kExitPaused) {
        release(l, udir);
        return Unit::Paused;
    }
    // Interrupted (a second Ctrl-C, or one that arrived before verify set its
    // handler: STATUS_CONTROL_C_EXIT): keep the lease and the slices for the
    // next start.
    if (rc == kExitAborted || (rc != 0 && g_stop.load() > 0)) {
        return Unit::Aborted;
    }
    std::string log;
    (void) read_file(udir / "verify.log", log);
    // Only a start failure sets err: a Windows NTSTATUS exit code is negative.
    std::fprintf(stderr,
                 "worker: verify failed on unit %llu (%s)\n%s\n",
                 static_cast<unsigned long long>(l.unit_id),
                 !err.empty() ? err.c_str() : ("exit code " + wk::exit_code_text(rc)).c_str(),
                 wk::redact(tail_lines(log, 12)).c_str());
    release(l, udir);
    drop(udir);
    return Unit::Failed;
}

} // namespace collatz::cli::worker_detail
