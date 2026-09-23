// The progress display and worker.log: start and finish, the redraw tick, the
// unit's progress file, the frontier from GET /api/stats and the state the
// display renders.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/progress.hpp"
#include "collatz/threading.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace collatz::cli::worker_detail {

namespace {

double seconds_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

} // namespace

void Worker::start_display() {
    t0_ = Clock::now();
    session_since_ = wk::local_time_text(std::time(nullptr), false);
    log_path_ = dir_ / "worker.log";
    std::error_code ec;
    // A line a minute is about 8 MiB in 50 days; keep one older file, in the
    // state directory, so that the rename replaces only a file the worker wrote.
    const std::uintmax_t size = fs::file_size(log_path_, ec);
    if (!ec && size > (std::uintmax_t{8} << 20)) {
        fs::rename(log_path_, state_dir_ / "worker.log.1", ec);
    }
    const char* ascii = std::getenv("COLLATZ_ASCII");
    wk::Terminal term;
    if (s_.display >= 0) {
        term = wk::open_terminal(ascii != nullptr && ascii[0] == '1');
    }
    const bool live = s_.display > 0 || (s_.display == 0 && term.ansi);
    msys_pty_ = term.msys_pty;
    screen_.start(live, live && term.utf8);
    display_on_ = true;
    status_ = "asking the server for work";
}

void Worker::finish_display(int rc) {
    if (!display_on_) {
        return;
    }
    unit_.active = false;
    status_countdown_ = false;
    switch (rc) {
    case kExitDone:
        status_ = "stopped";
        final_hint_ = "done";
        break;
    case kExitPaused:
        status_ = "paused";
        final_hint_ = "paused: run the same command to continue";
        break;
    case kExitAborted:
        status_ = "aborted";
        final_hint_ = "aborted: the lease is kept; run the same command to resume the unit";
        break;
    default:
        status_ = "stopped";
        final_hint_ = "stopped on an error (see above)";
        break;
    }
    if (screen_.live()) {
        screen_.close(wk::render(display_state(Clock::now()),
                                 wk::RenderOptions{wk::terminal_width(), screen_.utf8()}));
    }
    log_line("stop: " + final_hint_);
    wk::restore_terminal();
    display_on_ = false;
}

void Worker::tick() {
    if (!display_on_) {
        return;
    }
    const Clock::time_point now = Clock::now();
    if (now < next_tick_) {
        return;
    }
    next_tick_ = now + std::chrono::seconds(1);
    if (unit_.active) {
        refresh_unit();
    }
    maybe_fetch_frontier(now);
    const wk::DisplayState d = display_state(now);
    if (screen_.live()) {
        screen_.draw(wk::render(d, wk::RenderOptions{wk::terminal_width(), screen_.utf8()}));
    }
    if (unit_.active && now >= unit_.next_plain) {
        unit_.next_plain = now + std::chrono::minutes(1);
        const std::string line = wk::plain_progress_line(d);
        if (!screen_.live()) {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
        log_line(line);
    }
}

void Worker::hide_display() {
    screen_.suspend();
    if (frontier_thread_.joinable()) {
        frontier_thread_.join();
    }
}

void Worker::refresh_unit() {
    std::string text;
    collatz::progress::Snapshot snap;
    if (read_file(unit_.progress_file, text) && collatz::progress::parse_line(text, snap)) {
        unit_.snap = snap;
        unit_.have = true;
    }
    if (unit_.gpu && gpu_name_.empty() && unit_.scan_log) {
        std::string log;
        if (read_file(unit_.log_file, log)) {
            gpu_name_ = wk::gpu_device_from_log(log);
            // verify prints the line before its first slice; a long log has none.
            unit_.scan_log = gpu_name_.empty() && log.size() < (std::size_t{256} << 10);
        }
    }
}

void Worker::maybe_fetch_frontier(Clock::time_point now) {
    if (now < frontier_next_ || frontier_->busy.load()) {
        return;
    }
    if (frontier_thread_.joinable()) {
        frontier_thread_.join(); // finished: busy is false
    }
    frontier_next_ = now + std::chrono::minutes(5);
    frontier_->busy.store(true);
    const auto fetch =
        [fr = frontier_, curl = s_.curl, url = s_.server + "/api/stats", tmp = tmp_dir_] {
            bool ok = false;
            double lg = 0.0;
            std::string value;
            try {
                const wk::HttpResponse h = wk::http_request(curl, "GET", url, "", tmp, 15);
                if (h.status == 200) {
                    const std::optional<Json> doc = collatz::checkpoint::parse_json(h.body);
                    ok = doc && wk::stats_frontier(*doc, lg, value);
                }
            } catch (...) {
                ok = false; // shown as a failed check
            }
            {
                const std::lock_guard<std::mutex> lk(fr->m);
                fr->failed = !ok;
                if (ok) {
                    fr->have = true;
                    fr->log2 = lg;
                    fr->value = value;
                    fr->at = Clock::now();
                }
            }
            fr->busy.store(false);
        };
    try {
        frontier_thread_ = std::thread(fetch);
    } catch (const std::system_error&) {
        frontier_->busy.store(false); // no thread: try again in 5 minutes
    }
}

wk::DisplayState Worker::display_state(Clock::time_point now) {
    wk::DisplayState d;
    if (unit_.active ? unit_.gpu : s_.gpu) {
        d.device = gpu_name_.empty() ? "GPU" : "GPU " + gpu_name_;
    } else {
        const unsigned t = s_.threads != 0 ? s_.threads : collatz::hardware_threads();
        d.device = "CPU, " + std::to_string(t) + (t == 1 ? " thread" : " threads");
    }
    d.uptime_s = seconds_between(t0_, now);
    d.server = s_.server;
    d.running = unit_.active;
    if (unit_.active) {
        d.unit_id = unit_.id;
        d.block = wk::block_label(unit_.h, unit_.K);
        d.block_bits = unit_.K;
        d.started = unit_.started;
        d.have_progress = unit_.have;
        if (unit_.have) {
            d.progress = wk::block_progress(
                unit_.snap, unit_.K, seconds_between(unit_.t0, now), unit_.shown);
            unit_.shown = d.progress.fraction;
            // Not 100% before verify exits: the last tails, the merge and the
            // result file come after the last survivor is counted.
            d.progress.fraction = std::min(d.progress.fraction, 0.999);
        }
    } else {
        d.status = status_;
        if (status_countdown_) {
            d.status += "; next try in " +
                        wk::format_duration(std::max(0.0, seconds_between(now, status_until_)));
        }
    }
    d.session_blocks = session_blocks_;
    d.session_numbers = session_numbers_ + d.progress.session_numbers;
    d.session_since = session_since_;
    {
        const std::lock_guard<std::mutex> lk(frontier_->m);
        d.have_frontier = frontier_->have;
        d.frontier_log2 = frontier_->log2;
        d.frontier_value = frontier_->value;
        d.frontier_age_s = seconds_between(frontier_->at, now);
        d.frontier_failed = frontier_->failed;
    }
    d.last = last_;
    d.hint = hint_text();
    return d;
}

std::string Worker::hint_text() const {
    if (!final_hint_.empty()) {
        return final_hint_;
    }
    if (stop_requested()) {
        if (!unit_.active) {
            return "stopping";
        }
        // verify stops between slices, so a block on its last slice is finished
        // and submitted first.
        const bool last_slice = unit_.have && unit_.snap.slices_total - unit_.snap.slices_done <= 1;
        return last_slice ? "pausing after this block (its last slice runs); Ctrl-C again quits now"
                          : "pausing after the slice in progress (the block is kept); Ctrl-C "
                            "again quits now";
    }
    if (msys_pty_) {
        // mintty may not pass Ctrl-C on to a native program.
        return "to pause, create " + pause_file_.string() +
               (unit_.active ? " (the block's progress is kept)" : "");
    }
    return unit_.active ? "Ctrl-C pauses (the block's progress is kept and resumed next time)"
                        : "Ctrl-C stops the worker";
}

void Worker::log_line(const std::string& line) const {
    if (!display_on_) {
        return;
    }
    append_log(log_path_, line);
}

} // namespace collatz::cli::worker_detail
