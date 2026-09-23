#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/cli.hpp"
#include "collatz/u128.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

// worker is the client of the work-unit server in server/. It leases a block of
// at most 2^--block-bits numbers, runs `collatz verify --block H --K K ...
// --checkpoint DIR` on it in a child process with the K the server chose,
// submits the result JSON and repeats. A killed worker resumes its unit from
// DIR on the next start.
//
// Every request carries the server's worker token from --token-file or
// COLLATZ_WORKER_TOKEN; the worker never stores it, and server text, curl
// messages and log lines are printed with the token redacted. A share
// --audit-rate of the accepted units is audited by a low-priority `collatz
// certify --audit-seed` child alongside the next unit; the outcome goes to
// /api/audit.
//
// The request files, units, audits, lock and PAUSE file live in the directory
// collatz-worker next to the config file, which the worker makes itself and
// marks (wk::open_state_dir); it deletes or changes files only there, so a
// config file among other files leaves them alone.
//
// On a terminal a display of the block in progress stays at the bottom
// (collatz/worker_display.hpp). It is redrawn every second from the child's
// --progress-file and erased before any other output, so messages scroll above
// it. Without a terminal a progress line is printed once a minute. Progress and
// unit lines also go to worker.log next to the config file.

namespace collatz::cli::worker_detail {

bool parse_u64(std::string_view s, std::uint64_t& out) {
    collatz::u128 v = 0;
    if (!collatz::from_string(s, v) || v > ~std::uint64_t{0}) {
        return false;
    }
    out = static_cast<std::uint64_t>(v);
    return true;
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !in.bad();
}

/// Appends `line` with the local time to the log file `path`; does nothing when
/// `path` is empty. The line is redact()ed: it may quote server or curl text.
void append_log(const fs::path& path, const std::string& line) {
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::app);
    out << wk::local_time_text(std::time(nullptr), true) << ' ' << wk::redact(line) << '\n';
}

std::string reply_error(const Reply& r) {
    if (r.status == 0 || !r.error.empty() || !r.json) {
        return r.error;
    }
    // A JSON reply without an "error" member: its status and a body excerpt
    // (printable() redacts the token, also in its JSON-escaped form).
    return "HTTP " + std::to_string(r.status) + ": " + wk::printable(wk::json_dump(*r.json), 120);
}

std::string tail_lines(const std::string& text, std::size_t n) {
    std::size_t pos = text.size();
    while (n > 0 && pos > 0) {
        pos = text.rfind('\n', pos - 1);
        if (pos == std::string::npos) {
            return text;
        }
        --n;
    }
    return text.substr(pos == 0 ? 0 : pos + 1);
}

/// Raw text of a number member, so callers can parse integers exactly; "" when
/// the member is absent or not a number.
std::string num_of(const Json* obj, const char* key) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    return v != nullptr && v->type == Json::Type::Number ? v->text : std::string();
}

namespace {

// The default config path, the environment overrides and the flag
// combinations. Returns false after printing an error.
bool resolve_settings(Settings& s) {
    if (s.config_path.empty()) {
        s.config_path = wk::default_config_path();
        if (s.config_path.empty()) {
            std::fprintf(stderr,
                         "error: no config directory (set APPDATA, or pass --config FILE)\n");
            return false;
        }
    }
    for (const char c : s.config_path.string()) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            std::fprintf(stderr,
                         "error: the config path must be ASCII (verify takes its paths through "
                         "the ANSI command line); pass --config with an ASCII path to a new "
                         "directory, which the worker creates readable by you only\n");
            return false;
        }
    }
    if (const char* b = std::getenv("COLLATZ_WORKER_BACKOFF_MS"); b != nullptr && b[0] != '\0') {
        std::uint64_t ms = 0;
        if (parse_u64(b, ms) && ms >= 1 && ms <= 3600000) {
            s.backoff_ms = static_cast<unsigned>(ms);
        }
    }
    if (s.recheck_cpu && !s.gpu) {
        std::fprintf(
            stderr,
            "worker: --recheck-cpu goes with --gpu (a CPU worker runs every unit on the CPU)\n");
    }
    return true;
}

// The server URL: --server, else the one in the config file.
bool resolve_server(Settings& s, const WorkerArgs& wa, const wk::Config& cfg) {
    s.server = wa.server_arg.empty() ? cfg.server : wa.server_arg;
    while (!s.server.empty() && s.server.back() == '/') {
        s.server.pop_back();
    }
    if (s.server.empty()) {
        std::fprintf(stderr, "error: --server URL is required on the first run\n");
        print_usage();
        return false;
    }
    if (!wk::server_url_ok(s.server)) {
        std::fprintf(stderr,
                     "error: --server must be an https:// URL (http:// only for localhost); the "
                     "worker token travels in the request body\n");
        return false;
    }
    return true;
}

// The worker token from --token-file, else COLLATZ_WORKER_TOKEN; false after
// printing an error when neither gives one.
bool load_token(Settings& s, const WorkerArgs& wa) {
    std::string err;
    if (!wa.token_file.empty()) {
        if (!wk::read_worker_token(fs::path(wa.token_file), s.token, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return false;
        }
    } else if (const char* t = std::getenv("COLLATZ_WORKER_TOKEN"); t != nullptr && t[0] != '\0') {
        s.token = t;
        while (!s.token.empty() && (s.token.back() == '\n' || s.token.back() == '\r')) {
            s.token.pop_back();
        }
        if (!wk::valid_worker_token(s.token)) {
            std::fprintf(stderr,
                         "error: COLLATZ_WORKER_TOKEN is not a worker token (32 to 1024 printable "
                         "characters without spaces)\n");
            return false;
        }
    } else {
        std::fprintf(stderr,
                     "error: give the worker the server's token with --token-file PATH (a file "
                     "holding WORKER_TOKEN) or the environment variable COLLATZ_WORKER_TOKEN\n");
        return false;
    }
    return true;
}

// A result computed under a result-changing environment variable would be
// submitted as a regular result and caught only by a second run, so the worker
// refuses to start.
bool result_env_clear() {
    for (const char* name : collatz::checkpoint::result_env_names()) {
        if (const char* v = std::getenv(name); v != nullptr && v[0] != '\0') {
            std::fprintf(stderr,
                         "error: %s is set; it can change verify results, so the worker will not "
                         "run (unset it)\n",
                         name);
            return false;
        }
    }
    return true;
}

int run_worker(int argc, char** argv) {
    Settings s;
    WorkerArgs wa;
    if (const std::optional<int> rc = parse_worker_args(argc, argv, s, wa)) {
        return *rc;
    }
    if (!resolve_settings(s)) {
        return kExitError;
    }
    // Every request goes through curl, so a worker that cannot start it would
    // only retry; stop at once instead.
    if (!wk::program_found(s.curl)) {
        std::fprintf(stderr,
                     "error: cannot run curl: '%s' is no executable file%s; install curl or "
                     "pass --curl PATH\n",
                     wk::printable(s.curl).c_str(),
                     s.curl.find_first_of("/\\") == std::string::npos ? " on PATH" : "");
        return kExitError;
    }
    wk::Config cfg;
    std::string err;
    if (!wk::load_config(s.config_path, cfg, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return kExitError;
    }
    if (!resolve_server(s, wa, cfg)) {
        return kExitError;
    }
    if (wa.status) {
        // curl's response files go to the state directory's tmp/.
        if (!wk::make_private_dirs(s.config_path.parent_path(), err) ||
            !wk::open_state_dir(wk::state_dir(s.config_path), err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return kExitError;
        }
        return Worker(s).print_status();
    }
    if (!load_token(s, wa)) {
        return kExitError;
    }
    // From here on no server or curl text, and no log line, shows the token.
    wk::set_secret(s.token);
    if (!result_env_clear()) {
        return kExitError;
    }
    return Worker(s).run();
}

} // namespace

} // namespace collatz::cli::worker_detail

// COLLATZ_REGISTER_COMMAND names the function unqualified.
using collatz::cli::worker_detail::run_worker;

COLLATZ_REGISTER_COMMAND("worker", "work-unit client", run_worker)
