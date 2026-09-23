#ifndef COLLATZ_CLI_CMD_WORKER_SETTINGS_HPP
#define COLLATZ_CLI_CMD_WORKER_SETTINGS_HPP

// The settings of `collatz worker`, its command-line values and exit codes,
// shared by the src/cli/cmd_worker*.cpp pieces.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace collatz::cli::worker_detail {

namespace fs = std::filesystem;

// Same codes as `verify --checkpoint`: 0 done, 2 error, 4 paused, 130 aborted.
inline constexpr int kExitDone = 0;
inline constexpr int kExitError = 2;
inline constexpr int kExitPaused = 4;
inline constexpr int kExitAborted = 130;

// With --once the worker stops (exit 2) after this many failed tries to get a
// lease or to submit a result, instead of retrying until the server or curl
// works again.
inline constexpr unsigned kOnceTries = 3;

struct Settings {
    std::string server;
    unsigned threads = 0;
    bool gpu = false;
    bool recheck_cpu = false;    ///< --recheck-cpu: the CPU also takes re-checks of GPU results
    unsigned block_bits = 54;    ///< --block-bits: the block size asked for, as a power of two
    std::uint64_t max_units = 0; ///< 0 = no limit
    std::string curl = "curl";
    fs::path config_path;
    unsigned backoff_ms = 15000;
    /// The worker token, from --token-file or COLLATZ_WORKER_TOKEN.
    std::string token;
    /// The progress display: 1 --tui (always), -1 --plain (never), 0 on a terminal.
    int display = 0;
    /// --audit-rate: the share of accepted units audited with `certify --audit-seed`.
    double audit_rate = 0.05;
};

// Command-line values that are not Settings fields.
struct WorkerArgs {
    bool status = false;
    std::string server_arg;
    std::string token_file;
};

// Defined in cmd_worker_args.cpp.
void print_usage();
std::optional<int> parse_worker_args(int argc, char** argv, Settings& s, WorkerArgs& wa);

} // namespace collatz::cli::worker_detail

#endif // COLLATZ_CLI_CMD_WORKER_SETTINGS_HPP
