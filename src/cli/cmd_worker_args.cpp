// The `collatz worker` command line: the usage text and the argument parser.

#include "cmd_worker_internal.hpp"
#include "collatz/verify/bounds.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace collatz::cli::worker_detail {

void print_usage() {
    std::fprintf(
        stderr,
        "usage: collatz worker --server URL [--token-file PATH] [--threads T] [--gpu]\n"
        "                      [--recheck-cpu] [--block-bits N] [--once] [--max-units N]\n"
        "                      [--config FILE] [--curl PATH] [--tui | --plain] [--audit-rate R]\n"
        "       collatz worker --status [--server URL]\n"
        "  --server URL    work server (https://...); remembered in the config file\n"
        "  --token-file PATH  a file holding the server's worker token (or set\n"
        "                  COLLATZ_WORKER_TOKEN); the token is never read from the\n"
        "                  command line, where other processes can see it, and never stored\n"
        "  --threads T     verify threads (default 0 = all hardware threads; at most 4\n"
        "                  per hardware thread or 256, whichever is larger)\n"
        "  --gpu           run units on the GPU (OpenCL)\n"
        "  --recheck-cpu   with --gpu: also run re-checks of GPU results on the CPU when the\n"
        "                  server asks for that\n"
        "  --block-bits N  ask for blocks of 2^N numbers (default 54); the server may hand\n"
        "                  out a smaller block, and a larger one is handed back unrun;\n"
        "                  pick N so that a unit takes 1-10 minutes on this machine\n"
        "  --once          one unit, then exit (same as --max-units 1); exits with 2 after\n"
        "                  3 failed tries to get a lease or to submit its result\n"
        "  --max-units N   stop after N submitted units\n"
        "  --audit-rate R  audit this share of the accepted units (0 to 1, default 0.05):\n"
        "                  `collatz certify --audit-seed` re-derives a random sample of the\n"
        "                  unit's sieve decisions in GMP, on one CPU thread at low priority,\n"
        "                  while the next unit runs, and the outcome goes to the server\n"
        "  --config FILE   config file (default %%APPDATA%%\\collatz\\worker.json);\n"
        "                  the worker creates it readable by you only, and keeps its state\n"
        "                  (units, audits, request files) in collatz-worker/ next to it\n"
        "  --curl PATH     curl executable (default curl: on Windows the system\n"
        "                  directory's curl.exe, then PATH)\n"
        "  --status        print the server's status and exit\n"
        "  --tui           always draw the progress display (ANSI escapes), even when\n"
        "                  stdout is no terminal; by default it is drawn on a terminal only\n"
        "  --plain         no display: one progress line a minute and one line per unit\n"
        "                  (what output that is no terminal gets); COLLATZ_ASCII=1 draws\n"
        "                  the display's bar in ASCII\n"
        "Ctrl-C (or creating collatz-worker/PAUSE next to the config file) pauses: the\n"
        "unit in progress is saved and handed back; a second Ctrl-C quits at once and\n"
        "keeps the lease. Progress and unit lines are also appended to worker.log next to\n"
        "the config file.\n"
        "Exit codes: 0 done, 2 error, 4 paused, 130 aborted.\n");
}

// Returns the exit code when the command line ends the run (--help or an
// error), else nullopt.
std::optional<int> parse_worker_args(int argc, char** argv, Settings& s, WorkerArgs& wa) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        // An empty value (an unset shell variable) is refused rather than
        // read as "not given", which would fall back to the config file's
        // server or to COLLATZ_WORKER_TOKEN.
        const auto value = [&](std::string& dst) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::fprintf(stderr,
                             "error: %.*s needs a non-empty value\n",
                             static_cast<int>(a.size()),
                             a.data());
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--server") {
            if (!value(wa.server_arg)) {
                return kExitError;
            }
        } else if (a == "--threads" || a == "--max-units") {
            std::uint64_t n = 0;
            if (!value(v) || !parse_u64(v, n) || n > 0xFFFFFFFFu) {
                std::fprintf(stderr,
                             "error: %.*s needs a non-negative integer\n",
                             static_cast<int>(a.size()),
                             a.data());
                return kExitError;
            }
            if (a == "--threads") {
                // Passed on to every unit's verify (verify_args), which would
                // refuse a larger count and fail each unit.
                if (n > collatz::verify::max_threads()) {
                    std::fprintf(stderr,
                                 "error: --threads must be at most %u here (4 per hardware "
                                 "thread, at least 256; 0 = all hardware threads)\n",
                                 collatz::verify::max_threads());
                    return kExitError;
                }
                s.threads = static_cast<unsigned>(n);
            } else {
                s.max_units = n;
            }
        } else if (a == "--once") {
            s.max_units = 1;
        } else if (a == "--gpu") {
            s.gpu = true;
        } else if (a == "--recheck-cpu") {
            s.recheck_cpu = true;
        } else if (a == "--block-bits") {
            std::uint64_t n = 0;
            if (!value(v) || !parse_u64(v, n) || n < 32 || n > 64) {
                std::fprintf(stderr, "error: --block-bits needs an integer from 32 to 64\n");
                return kExitError;
            }
            s.block_bits = static_cast<unsigned>(n);
        } else if (a == "--audit-rate") {
            if (!value(v)) {
                return kExitError;
            }
            char* end = nullptr;
            const double r = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(r >= 0.0 && r <= 1.0)) {
                std::fprintf(stderr, "error: --audit-rate needs a number from 0 to 1\n");
                return kExitError;
            }
            s.audit_rate = r;
        } else if (a == "--config") {
            if (!value(v)) {
                return kExitError;
            }
            s.config_path = fs::absolute(fs::path(v));
        } else if (a == "--curl") {
            if (!value(s.curl)) {
                return kExitError;
            }
        } else if (a == "--token-file") {
            if (!value(v)) {
                return kExitError;
            }
            wa.token_file = v;
        } else if (a == "--token" || a.rfind("--token=", 0) == 0) {
            // The argument is not echoed: it may be the token.
            std::fprintf(stderr,
                         "error: the worker token is not taken on the command line, where other "
                         "processes can read it; put it in a file and pass --token-file PATH, or "
                         "set COLLATZ_WORKER_TOKEN\n");
            return kExitError;
        } else if (a == "--status") {
            wa.status = true;
        } else if (a == "--tui" || a == "--plain") {
            const int want = a == "--tui" ? 1 : -1;
            if (s.display != 0 && s.display != want) {
                std::fprintf(stderr, "error: give --tui or --plain, not both\n");
                return kExitError;
            }
            s.display = want;
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return kExitDone;
        } else {
            std::fprintf(
                stderr, "error: unknown argument '%.*s'\n", static_cast<int>(a.size()), a.data());
            print_usage();
            return kExitError;
        }
    }
    return std::nullopt;
}

} // namespace collatz::cli::worker_detail
