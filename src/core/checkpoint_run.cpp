// The checkpointed run: the pause flag, the slice loop with its pause checks,
// and the merge of a finished run into merged.json.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/progress.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace collatz::checkpoint {

namespace fs = std::filesystem;

using namespace detail;

// The SIGINT handler touches this flag; only a lock-free atomic is
// async-signal-safe.
static_assert(std::atomic<int>::is_always_lock_free,
              "pause_flag is written from a signal handler and must be lock-free");

std::atomic<int>& pause_flag() noexcept {
    static std::atomic<int> flag{0};
    return flag;
}

namespace {

std::string fingerprint_json(const Fingerprint& fp, const char* indent) {
    std::string s = "{";
    bool first = true;
    for (const auto& [k, v] : fp) {
        s += first ? "\n" : ",\n";
        first = false;
        s += indent;
        s += "\"" + json_escape(k) + "\": \"" + json_escape(v) + "\"";
    }
    s += '\n';
    s += std::string(indent).substr(2);
    s += '}';
    return s;
}

std::string run_json(const RunConfig& c) {
    std::string s = "{\n";
    s += "  \"schema\": \"collatz-checkpoint-run/1\",\n";
    s += "  \"slices\": " + std::to_string(c.K) + ",\n";
    s += "  \"slice_lo\": " + std::to_string(c.lo) + ",\n";
    s += "  \"slice_hi\": " + std::to_string(c.hi) + ",\n";
    s += "  \"command\": \"" + json_escape(c.command_line) + "\",\n";
    s += "  \"fingerprint\": " + fingerprint_json(c.fingerprint, "    ") + "\n";
    s += "}\n";
    return s;
}

std::string checkpoint_block(const RunConfig& c, std::uint64_t slice_c, double wall) {
    char wbuf[64];
    std::snprintf(wbuf, sizeof(wbuf), "%.3f", wall);
    std::string s = "\"checkpoint\": {\n";
    s += "    \"schema\": \"collatz-checkpoint-slice/1\",\n";
    s += "    \"c\": " + std::to_string(slice_c) + ", \"K\": " + std::to_string(c.K) + ",\n";
    s += "    \"wall_seconds\": " + std::string(wbuf) + ",\n";
    s += "    \"fingerprint\": " + fingerprint_json(c.fingerprint, "      ") + "\n  }";
    return s;
}

std::string fmt_duration(double sec) {
    char b[64];
    if (sec >= 5400.0) {
        std::snprintf(b, sizeof(b), "%.1f h", sec / 3600.0);
    } else if (sec >= 60.0) {
        std::snprintf(b, sizeof(b), "%.1f min", sec / 60.0);
    } else {
        std::snprintf(b, sizeof(b), "%.0f s", sec);
    }
    return b;
}

// Runs and saves the slices in `todo`. Returns Complete once all of them are
// done, Paused on a pause request, or Error.
Outcome run_slices(const RunConfig& cfg,
                   const fs::path& dir,
                   const SliceRunner& runner,
                   const std::vector<std::uint64_t>& todo,
                   std::uint64_t range_n,
                   std::uint64_t resumed_n,
                   std::vector<Slice>& done,
                   std::string& err) {
    using Clock = std::chrono::steady_clock;
    double session_sec = 0.0;
    unsigned session_n = 0;
    const auto paused_msg = [&] {
        err = std::to_string(done.size()) + " of " + std::to_string(range_n) + " slices done, " +
              std::to_string(range_n - done.size()) + " left";
        return Outcome::Paused;
    };
    for (const std::uint64_t c : todo) {
        if (pause_flag().load() != 0 || pause_file_present(dir)) {
            return paused_msg();
        }
        verify::Options so = cfg.opts;
        so.step1.case_c = c;
        so.step1.case_k = cfg.K;
        std::string backend = cfg.meta.backend;
        const auto t0 = Clock::now();
        verify::Result r;
        try {
            r = runner(so, backend);
        } catch (const std::exception& e) {
            err = std::string("slice ") + std::to_string(c) + " failed: " + e.what();
            return Outcome::Error;
        }
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        JsonMeta m = cfg.meta;
        m.backend = backend;
        const std::string doc = result_json(m, so, r, checkpoint_block(cfg, c, wall));
        if (!atomic_write(dir / slice_file_name(c, cfg.K), doc, err)) {
            return Outcome::Error;
        }
        Slice s;
        std::string perr;
        if (!parse_slice(doc, s, perr)) {
            err = "internal: freshly written slice does not parse: " + perr;
            return Outcome::Error;
        }
        s.source = (dir / slice_file_name(c, cfg.K)).string();
        done.push_back(std::move(s));
        progress::set_slices(done.size(), range_n, resumed_n);
        session_sec += wall;
        ++session_n;
        const std::uint64_t left = range_n - done.size();
        const double eta = session_sec / session_n * static_cast<double>(left);
        std::fprintf(stderr,
                     "slice %llu/%llu done (%.1f s), %llu left, ~%s remaining  [case %llu/%llu]\n",
                     static_cast<unsigned long long>(done.size()),
                     static_cast<unsigned long long>(range_n),
                     wall,
                     static_cast<unsigned long long>(left),
                     fmt_duration(eta).c_str(),
                     static_cast<unsigned long long>(c),
                     static_cast<unsigned long long>(cfg.K));
        std::fflush(stderr);
    }
    return Outcome::Complete;
}

// After the last slice: RangeDone for a --slice-range share, else the merged
// Result, also written to DIR/merged.json.
Outcome finish_run(const RunConfig& cfg,
                   const fs::path& dir,
                   const std::vector<Slice>& done,
                   verify::Result& merged,
                   std::string& err,
                   MergeInfo* info) {
    if (cfg.lo != 0 || cfg.hi != cfg.K) {
        err = "slices " + std::to_string(cfg.lo) + ".." + std::to_string(cfg.hi - 1) + " of " +
              std::to_string(cfg.K) + " are done";
        return Outcome::RangeDone;
    }
    MergeInfo mi;
    if (!merge(done, merged, err, &mi)) {
        return Outcome::Error;
    }
    if (info != nullptr) {
        *info = mi;
    }
    JsonMeta m = cfg.meta;
    m.backend = mi.backend; // the backend recorded in the slices
    m.commit = mi.commit;   // equal to cfg.meta.commit: scan_slices redoes any other
    m.unwalked_depth = mi.depth;
    verify::Options mo = cfg.opts;
    mo.step1.case_c = 0;
    mo.step1.case_k = 1;
    std::string block = "\"checkpoint\": {\"schema\": \"collatz-checkpoint-merged/1\", "
                        "\"merged_slices\": " +
                        std::to_string(cfg.K) + "}";
    if (!atomic_write(dir / "merged.json", result_json(m, mo, merged, block), err)) {
        return Outcome::Error;
    }
    return Outcome::Complete;
}

} // namespace

Outcome run(RunConfig cfg,
            const SliceRunner& runner,
            verify::Result& merged,
            std::string& err,
            MergeInfo* info) {
    const fs::path dir = fs::path(cfg.dir);
    if (!prepare_dir(cfg, dir, err)) {
        return Outcome::Error;
    }
    const fs::path run_path = dir / "run.json";
    std::error_code ec;

    if (fs::exists(run_path, ec)) {
        if (!adopt_run_json(cfg, run_path, err)) {
            return Outcome::Error;
        }
    } else {
        init_new_run(cfg);
    }
    if (cfg.hi == 0) {
        cfg.hi = cfg.K;
    }
    if (!check_slicing(cfg, err)) {
        return Outcome::Error;
    }
    if (!fs::exists(run_path, ec)) {
        if (!atomic_write(run_path, run_json(cfg), err)) {
            return Outcome::Error;
        }
    }
    // Stale *.tmp / PAUSE are gone: the CLI may start watching DIR/PAUSE now
    // without mistaking the old file for a new request.
    if (cfg.on_ready) {
        cfg.on_ready();
    }

    std::vector<Slice> done;
    std::vector<std::uint64_t> todo;
    if (!scan_slices(cfg, dir, done, todo, err)) {
        return Outcome::Error;
    }
    const std::uint64_t range_n = cfg.hi - cfg.lo;
    // --progress-file: the slices found on disk count as done before this
    // process started, so the worker leaves them out of its speed.
    const std::uint64_t resumed_n = done.size();
    progress::set_slices(done.size(), range_n, resumed_n);
    std::fprintf(stderr,
                 "checkpoint: %s: slices %llu..%llu of %llu: %llu done, %llu to run "
                 "(Ctrl-C or create %s to pause after the current slice)\n",
                 cfg.dir.c_str(),
                 static_cast<unsigned long long>(cfg.lo),
                 static_cast<unsigned long long>(cfg.hi - 1),
                 static_cast<unsigned long long>(cfg.K),
                 static_cast<unsigned long long>(done.size()),
                 static_cast<unsigned long long>(todo.size()),
                 (dir / "PAUSE").string().c_str());

    const Outcome oc = run_slices(cfg, dir, runner, todo, range_n, resumed_n, done, err);
    if (oc != Outcome::Complete) {
        return oc;
    }
    return finish_run(cfg, dir, done, merged, err, info);
}

} // namespace collatz::checkpoint
