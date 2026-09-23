#include "cmd_verify_internal.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdio>
#include <string>
#include <string_view>

// The loop over the verify arguments and the range and sieve option groups;
// cmd_verify_parse_gpu_output.cpp holds the GPU and output groups.

namespace collatz::cli::verify_detail {

bool parse_uint(std::string_view s, unsigned& out) {
    collatz::u128 v = 0;
    if (!collatz::from_string(s, v) || v > 0xFFFFFFFFu) {
        return false;
    }
    out = static_cast<unsigned>(v);
    return true;
}

bool need_uint(int argc, char** argv, int& i, unsigned& dst) {
    if (i + 1 >= argc || !parse_uint(argv[i + 1], dst)) {
        print_usage();
        return false;
    }
    ++i;
    return true;
}

namespace {

// --N/--K, --block, --record-threshold, --assumption-source and --threads.
Arg parse_range_arg(std::string_view a,
                    int argc,
                    char** argv,
                    int& i,
                    collatz::verify::Options& opts,
                    VerifyArgs& va) {
    if (a == "--N" || a == "--K") {
        if (!need_uint(argc, argv, i, opts.N)) {
            return Arg::Bad;
        }
        va.have_N = true;
    } else if (a == "--block") {
        collatz::u128 v = 0;
        if (i + 1 >= argc || !collatz::from_string(argv[i + 1], v)) {
            std::fprintf(stderr, "error: --block needs a decimal integer below 2^128\n");
            return Arg::Bad;
        }
        ++i;
        opts.block_h = v;
        va.have_block = true;
    } else if (a == "--record-threshold") {
        if (i + 1 < argc && std::string_view(argv[i + 1]) == "none") {
            ++i;
            va.record_threshold_off = true;
            opts.record_threshold.clear();
            return Arg::Ok;
        }
        std::string canon;
        if (i + 1 >= argc || !collatz::canonical_decimal(argv[i + 1], canon)) {
            std::fprintf(stderr, "error: --record-threshold needs a decimal integer or none\n");
            return Arg::Bad;
        }
        ++i;
        opts.record_threshold = canon;
        va.record_threshold_off = false;
    } else if (a == "--assumption-source") {
        if (i + 1 >= argc || argv[i + 1][0] == '\0') {
            std::fprintf(stderr, "error: --assumption-source needs a non-empty text\n");
            return Arg::Bad;
        }
        opts.assumption_source = argv[++i];
    } else if (a == "--threads") {
        if (!need_uint(argc, argv, i, opts.threads)) {
            return Arg::Bad;
        }
        // The run sizes per-thread buffers and spawns threads by this count.
        if (opts.threads > collatz::verify::max_threads()) {
            std::fprintf(stderr,
                         "error: --threads must be at most %u here (4 per hardware thread, at "
                         "least 256; 0 = all %u hardware threads)\n",
                         collatz::verify::max_threads(),
                         collatz::hardware_threads());
            return Arg::Bad;
        }
        va.have_threads = true;
    } else {
        return Arg::Unknown;
    }
    return Arg::Ok;
}

// Sieve and kernel options: --A, --B, --window-*, --bitvectors, --kernel,
// --mod3t and the --no-* switches.
Arg parse_sieve_arg(std::string_view a,
                    int argc,
                    char** argv,
                    int& i,
                    collatz::verify::Options& opts,
                    VerifyArgs& va) {
    if (a == "--A") {
        if (!need_uint(argc, argv, i, opts.A)) {
            return Arg::Bad;
        }
        va.have_A = true;
    } else if (a == "--B") {
        if (!need_uint(argc, argv, i, opts.B)) {
            return Arg::Bad;
        }
        va.have_B = true;
    } else if (a == "--window-chains") {
        if (!need_uint(argc, argv, i, opts.window_chains)) {
            return Arg::Bad;
        }
    } else if (a == "--window-templates-D") {
        if (!need_uint(argc, argv, i, opts.window_templates_D)) {
            return Arg::Bad;
        }
    } else if (a == "--bitvectors") {
        if (!need_uint(argc, argv, i, opts.num_bitvectors)) {
            return Arg::Bad;
        }
        // Each bitvector holds 2^B bits, so the count sizes the Step-2 tables.
        if (opts.num_bitvectors > collatz::verify::kMaxBitvectors) {
            std::fprintf(stderr,
                         "error: --bitvectors must be at most %u\n",
                         collatz::verify::kMaxBitvectors);
            return Arg::Bad;
        }
    } else if (a == "--kernel") {
        if (i + 1 >= argc) {
            print_usage();
            return Arg::Bad;
        }
        // table16 is the only Step-3 kernel.
        if (std::string_view(argv[++i]) != "table16") {
            std::fprintf(stderr, "error: --kernel must be table16\n");
            return Arg::Bad;
        }
    } else if (a == "--no-partial-oee") {
        opts.partial_oee = false;
    } else if (a == "--no-bv-cache") {
        opts.bv_cache = false;
    } else if (a == "--mod3t") {
        if (!need_uint(argc, argv, i, opts.step1.mod3t)) {
            return Arg::Bad;
        }
    } else {
        return Arg::Unknown;
    }
    return Arg::Ok;
}

} // namespace

// Returns 0, or the exit code after printing the error. -h or --help stops the
// scan with va.help set.
int parse_args(int argc, char** argv, collatz::verify::Options& opts, VerifyArgs& va) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-h" || a == "--help") {
            va.help = true;
            return 0;
        }
        Arg res = parse_range_arg(a, argc, argv, i, opts, va);
        if (res == Arg::Unknown) {
            res = parse_sieve_arg(a, argc, argv, i, opts, va);
        }
        if (res == Arg::Unknown) {
            res = parse_gpu_arg(a, va);
        }
        if (res == Arg::Unknown) {
            res = parse_output_arg(a, argc, argv, i, va);
        }
        if (res == Arg::Bad) {
            return 2;
        }
        if (res == Arg::Unknown) {
            std::fprintf(
                stderr, "error: unknown argument '%.*s'\n", static_cast<int>(a.size()), a.data());
            print_usage();
            return 2;
        }
    }
    return 0;
}

} // namespace collatz::cli::verify_detail
