// The checkpoint directory: slice file names, the cleanup of an earlier
// session, the scan for finished slices, and load_slices for `collatz merge`.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace collatz::checkpoint {

namespace fs = std::filesystem;

std::string slice_file_name(std::uint64_t c, std::uint64_t K) {
    return "slice_" + std::to_string(c) + "_of_" + std::to_string(K) + ".json";
}

namespace {

// Parse "slice_<c>_of_<K>.json".
bool parse_slice_name(const std::string& name, std::uint64_t& c, std::uint64_t& K) {
    unsigned long long cc = 0;
    unsigned long long kk = 0;
    char tail[8] = {0};
    if (std::sscanf(name.c_str(), "slice_%llu_of_%llu.%7s", &cc, &kk, tail) != 3) {
        return false;
    }
    if (std::string(tail) != "json" || slice_file_name(cc, kk) != name) {
        return false;
    }
    c = cc;
    K = kk;
    return true;
}

// A file this program writes in a checkpoint DIR: run.json, merged.json, a
// slice file, or the pause request PAUSE.
bool is_checkpoint_name(const std::string& name) {
    std::uint64_t c = 0;
    std::uint64_t K = 0;
    return name == "run.json" || name == "merged.json" || name == "PAUSE" ||
           parse_slice_name(name, c, K);
}

// The temp file atomic_write leaves when a run is killed while writing one of
// those files: "<name>.tmp".
bool is_checkpoint_temp(const std::string& name) {
    const std::string tmp = ".tmp";
    if (name.size() <= tmp.size() || name.compare(name.size() - tmp.size(), tmp.size(), tmp) != 0) {
        return false;
    }
    const std::string base = name.substr(0, name.size() - tmp.size());
    return base != "PAUSE" && is_checkpoint_name(base);
}

// The first entry of DIR that this program does not write, or "" when there is
// none (an empty DIR, or one holding only checkpoint files).
std::string foreign_entry(const fs::path& dir, std::error_code& ec) {
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (!is_checkpoint_name(name) && !is_checkpoint_temp(name)) {
            return name;
        }
    }
    return {};
}

// --checkpoint-force: drop only the files this feature writes.
void remove_previous_run(const RunConfig& cfg, const fs::path& dir) {
    std::error_code ec;
    unsigned removed = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        std::error_code rec;
        if (name != "PAUSE" && (is_checkpoint_name(name) || is_checkpoint_temp(name)) &&
            fs::remove(e.path(), rec)) {
            ++removed;
        }
    }
    std::fprintf(stderr,
                 "checkpoint: --checkpoint-force: removed %u previous checkpoint file(s) from %s\n",
                 removed,
                 cfg.dir.c_str());
}

// The partial writes of a killed run. Other *.tmp files are not this
// program's, so they stay.
void remove_partial_files(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (!is_checkpoint_temp(name)) {
            continue;
        }
        std::error_code rec;
        if (fs::remove(e.path(), rec)) {
            std::fprintf(stderr, "checkpoint: removed partial file %s\n", name.c_str());
        } else if (rec) {
            std::fprintf(stderr,
                         "checkpoint: warning: cannot remove partial file %s (%s)\n",
                         name.c_str(),
                         rec.message().c_str());
        }
    }
}

// The run itself as a Slice, for run_params_diff with each file: the options,
// depth, GPU flag and commit that run_slices writes into every slice.
Slice run_reference(const RunConfig& cfg) {
    Slice r;
    r.k = cfg.K;
    r.N = cfg.opts.N;
    r.block_h = cfg.opts.block_h;
    r.unwalked_depth =
        cfg.meta.unwalked_depth ? *cfg.meta.unwalked_depth : verify::unwalked_depth(cfg.opts);
    r.opts = cfg.opts;
    r.gpu = cfg.meta.use_gpu;
    r.commit = cfg.meta.commit;
    r.fingerprint = cfg.fingerprint;
    r.source = "this run";
    return r;
}

// Why slice file `s`, found as slice c, is not a slice of this run: the
// run_params_diff lines (file vs run), then the slice index, GPU flag and
// fingerprint. Empty when it is one.
std::vector<std::string> slice_mismatch(const Slice& s, const Slice& run, std::uint64_t c) {
    std::vector<std::string> d = detail::run_params_diff(s, run);
    const auto U = [](std::uint64_t v) { return std::to_string(v); };
    if (s.c != c || s.k != run.k) {
        d.push_back("case: " + U(s.c) + "/" + U(s.k) + " vs " + U(c) + "/" + U(run.k));
    }
    if (s.gpu != run.gpu) {
        d.push_back(std::string("gpu: ") + (s.gpu ? "true" : "false") + " vs " +
                    (run.gpu ? "true" : "false"));
    }
    if (s.fingerprint.empty() && !run.fingerprint.empty()) {
        d.emplace_back("no checkpoint fingerprint in the file");
    } else {
        for (const std::string& f : fingerprint_diff(s.fingerprint, run.fingerprint)) {
            d.push_back("fingerprint " + f);
        }
    }
    return d;
}

} // namespace

namespace detail {

bool pause_file_present(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "PAUSE", ec);
}

// Creates DIR and removes what an earlier session left behind: the partial
// files of this program, an old PAUSE request and, with --checkpoint-force,
// the previous run. A non-empty DIR without run.json is refused unless
// --checkpoint-force, so a mistyped DIR does not get checkpoint files among
// the user's own.
bool prepare_dir(const RunConfig& cfg, const fs::path& dir, std::string& err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        err = "cannot create checkpoint directory '" + cfg.dir + "'";
        return false;
    }
    if (!fs::exists(dir / "run.json", ec)) {
        const std::string other = foreign_entry(dir, ec);
        if (ec) {
            err = "cannot list checkpoint directory '" + cfg.dir + "' (" + ec.message() + ")";
            return false;
        }
        if (!other.empty() && !cfg.force) {
            err = "checkpoint directory '" + cfg.dir + "' is not empty and has no run.json, " +
                  "so verify --checkpoint did not make it (it holds '" + other +
                  "'); use a new or empty directory, or add --checkpoint-force to use it " +
                  "(only this program's files in it are removed)";
            return false;
        }
    }
    if (cfg.force) {
        remove_previous_run(cfg, dir);
    }
    remove_partial_files(dir);
    // A PAUSE that stays would pause every start, so a failed remove is an
    // error with its reason.
    if (pause_file_present(dir)) {
        std::error_code rec;
        fs::remove(dir / "PAUSE", rec);
        if (rec) {
            err = "cannot remove the old pause request " + (dir / "PAUSE").string() + " (" +
                  rec.message() + "), so the run would pause at once; remove it and run the " +
                  "same command again";
            return false;
        }
        std::fprintf(stderr, "checkpoint: removed old PAUSE file (resuming)\n");
    }
    return true;
}

// Sorts slices lo..hi-1 into finished ones (a valid file of this run) and
// ones still to run. A file of other parameters is redone, as an unreadable
// one is; a path that is not a regular file of at most kMaxReadBytes is an
// error, because this program never writes one and redoing it would replace it.
bool scan_slices(const RunConfig& cfg,
                 const fs::path& dir,
                 std::vector<Slice>& done,
                 std::vector<std::uint64_t>& todo,
                 std::string& err) {
    const Slice run = run_reference(cfg);
    std::error_code ec;
    for (std::uint64_t c = cfg.lo; c < cfg.hi; ++c) {
        const fs::path p = dir / slice_file_name(c, cfg.K);
        if (!fs::exists(p, ec)) {
            todo.push_back(c);
            continue;
        }
        std::string serr;
        if (!readable_file(p, kMaxReadBytes, serr)) {
            err = p.string() + ": " + serr +
                  "; verify --checkpoint writes only regular slice files, so move it away and "
                  "run the same command again";
            return false;
        }
        Slice s;
        if (!read_slice_file(p.string(), s, serr)) {
            std::fprintf(stderr,
                         "checkpoint: %s is unusable (%s); redoing slice %llu\n",
                         p.filename().string().c_str(),
                         serr.c_str(),
                         static_cast<unsigned long long>(c));
            todo.push_back(c);
            continue;
        }
        const std::vector<std::string> diff = slice_mismatch(s, run, c);
        if (!diff.empty()) {
            std::string list;
            for (const std::string& d : diff) {
                list += (list.empty() ? "" : "; ") + d;
            }
            std::fprintf(stderr,
                         "checkpoint: %s does not match this run's parameters (file vs this "
                         "run: %s); redoing slice %llu\n",
                         p.filename().string().c_str(),
                         list.c_str(),
                         static_cast<unsigned long long>(c));
            todo.push_back(c);
            continue;
        }
        done.push_back(std::move(s));
    }
    return true;
}

} // namespace detail

bool load_slices(const std::vector<std::string>& paths, std::vector<Slice>& out, std::string& err) {
    std::error_code ec;
    for (const std::string& p : paths) {
        const fs::path fp = fs::path(p);
        if (!fs::is_directory(fp, ec)) {
            err = "'" + p + "' is not a checkpoint directory";
            return false;
        }
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(fp, ec)) {
            std::uint64_t c = 0;
            std::uint64_t K = 0;
            if (parse_slice_name(e.path().filename().string(), c, K)) {
                files.push_back(e.path());
            }
        }
        if (files.empty()) {
            err = "no slice_*_of_*.json files in '" + p + "'";
            return false;
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            Slice s;
            std::string serr;
            if (!read_slice_file(f.string(), s, serr)) {
                err = f.string() + ": " + serr;
                return false;
            }
            out.push_back(std::move(s));
        }
    }
    return true;
}

} // namespace collatz::checkpoint
