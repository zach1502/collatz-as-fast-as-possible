#ifndef COLLATZ_CLI_CMD_VERIFY_INTERNAL_HPP
#define COLLATZ_CLI_CMD_VERIFY_INTERNAL_HPP

// Private declarations shared by the src/cli/cmd_verify*.cpp pieces of the
// verify command: the command-line state VerifyArgs, the result of an option
// group's parser and the functions that the pieces call across files.

#include "collatz/progress.hpp"
#include "collatz/verify.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

// CMakeLists.txt defines the commit at configure time; a compile outside CMake
// reports "unknown".
#ifndef COLLATZ_GIT_COMMIT
#define COLLATZ_GIT_COMMIT "unknown"
#endif

namespace collatz::cli::verify_detail {

// Command-line state that does not live in verify::Options. The have_* fields
// record options given explicitly, which the N-dependent defaults leave alone.
struct VerifyArgs {
    bool help = false; // -h or --help: print the usage to stdout, exit 0
    bool have_N = false;
    bool have_A = false;
    bool have_B = false;
    bool have_threads = false;
    bool use_gpu = false;
    std::string json_path;
    std::string progress_path;
    std::string checkpoint_dir;
    std::uint64_t ck_slices = 0; // 0: the default, or the K in DIR/run.json
    std::uint64_t ck_lo = 0;     // --slice-range lo:hi; hi 0 means K
    std::uint64_t ck_hi = 0;
    bool ck_range = false;
    bool ck_force = false;
    bool have_block = false;
    // --record-threshold none: no list, not even the block default.
    bool record_threshold_off = false;
};

// Result of one option group's parser. Unknown hands the argument to the next
// group; Bad means the error is already printed and the exit code is 2.
enum class Arg { Unknown, Ok, Bad };

// Defined in cmd_verify_usage.cpp. stderr after a usage error, stdout for --help.
void print_usage(std::FILE* out = stderr);

// Defined in cmd_verify_parse.cpp.
bool parse_uint(std::string_view s, unsigned& out);
bool need_uint(int argc, char** argv, int& i, unsigned& dst);
int parse_args(int argc, char** argv, collatz::verify::Options& opts, VerifyArgs& va);

// Defined in cmd_verify_parse_gpu_output.cpp.
Arg parse_gpu_arg(std::string_view a, VerifyArgs& va);
Arg parse_output_arg(std::string_view a, int argc, char** argv, int& i, VerifyArgs& va);

// Defined in cmd_verify_check.cpp.
int check_args(collatz::verify::Options& opts, VerifyArgs& va);

// Defined in cmd_verify_defaults.cpp.
void set_cli_defaults(collatz::verify::Options& opts);
void apply_auto_a(collatz::verify::Options& opts, const VerifyArgs& va);
void apply_auto_b(collatz::verify::Options& opts, const VerifyArgs& va);

// Defined in cmd_verify_run.cpp.
void print_gpu_device();
collatz::verify::Result
run_backend(const collatz::verify::Options& o, bool use_gpu, std::string& be);
void start_progress_writer(std::optional<collatz::progress::FileWriter>& writer,
                           const std::string& path);
std::optional<int> run_checkpointed(const collatz::verify::Options& opts,
                                    const VerifyArgs& va,
                                    int argc,
                                    char** argv,
                                    collatz::verify::Result& r,
                                    std::string& backend_str);

// Defined in cmd_verify_print.cpp.
void print_result(const collatz::verify::Options& opts,
                  const VerifyArgs& va,
                  const collatz::verify::Result& r,
                  const char* backend);

} // namespace collatz::cli::verify_detail

#endif // COLLATZ_CLI_CMD_VERIFY_INTERNAL_HPP
