#include "cmd_verify_internal.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// The GPU option group and the output and checkpoint option group of the verify
// command line.

namespace collatz::cli::verify_detail {

// --gpu.
Arg parse_gpu_arg(std::string_view a, VerifyArgs& va) {
    if (a != "--gpu") {
        return Arg::Unknown;
    }
    va.use_gpu = true;
    return Arg::Ok;
}

namespace {

// The path after `option` into `dst`. An empty value is an error: the run
// treats an empty path as "not given", so a script's unset variable would
// otherwise turn the option off with exit 0.
Arg need_path(std::string_view option, int argc, char** argv, int& i, std::string& dst) {
    if (i + 1 >= argc) {
        print_usage();
        return Arg::Bad;
    }
    if (argv[i + 1][0] == '\0') {
        std::fprintf(stderr,
                     "error: %.*s needs a non-empty path\n",
                     static_cast<int>(option.size()),
                     option.data());
        return Arg::Bad;
    }
    dst = argv[++i];
    return Arg::Ok;
}

} // namespace

// --json, --progress-file and the --checkpoint options.
Arg parse_output_arg(std::string_view a, int argc, char** argv, int& i, VerifyArgs& va) {
    if (a == "--json") {
        return need_path(a, argc, argv, i, va.json_path);
    }
    if (a == "--progress-file") {
        // Progress for `collatz worker` (collatz/progress.hpp). Only the file
        // is written; the Result does not change.
        return need_path(a, argc, argv, i, va.progress_path);
    }
    if (a == "--checkpoint") {
        return need_path(a, argc, argv, i, va.checkpoint_dir);
    }
    if (a == "--slices") {
        collatz::u128 v = 0;
        if (i + 1 >= argc || !collatz::from_string(argv[i + 1], v) || v == 0 || v > 1024u ||
            (v & (v - 1)) != 0) {
            std::fprintf(stderr, "error: --slices must be a power of two in [1, 1024]\n");
            return Arg::Bad;
        }
        ++i;
        va.ck_slices = static_cast<std::uint64_t>(v);
    } else if (a == "--slice-range") {
        if (i + 1 >= argc) {
            print_usage();
            return Arg::Bad;
        }
        const std::string_view rv = argv[++i];
        const auto colon = rv.find(':');
        collatz::u128 lo = 0;
        collatz::u128 hi = 0;
        if (colon == std::string_view::npos || !collatz::from_string(rv.substr(0, colon), lo) ||
            !collatz::from_string(rv.substr(colon + 1), hi) || lo >= hi || hi > 1024u) {
            std::fprintf(stderr, "error: --slice-range must be a:b with 0 <= a < b <= K\n");
            return Arg::Bad;
        }
        va.ck_lo = static_cast<std::uint64_t>(lo);
        va.ck_hi = static_cast<std::uint64_t>(hi);
        va.ck_range = true;
    } else if (a == "--checkpoint-force") {
        va.ck_force = true;
    } else {
        return Arg::Unknown;
    }
    return Arg::Ok;
}

} // namespace collatz::cli::verify_detail
