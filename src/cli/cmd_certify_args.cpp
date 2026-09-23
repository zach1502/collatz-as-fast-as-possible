// The argument parsing, checks and defaults of `collatz certify`; see
// cmd_certify.cpp.

#include "cmd_certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace collatz::cli::cmd_certify {
namespace {

bool parse_uint(std::string_view s, unsigned& out) {
    collatz::u128 v = 0;
    if (!collatz::from_string(s, v) || v > 0xFFFFFFFFu) {
        return false;
    }
    out = static_cast<unsigned>(v);
    return true;
}

} // namespace

// Returns 0, or the exit code after printing the error. --help or -h stops the
// parse with ca.help set.
int parse_certify_args(int argc, char** argv, collatz::certify::Options& opts, CertifyArgs& ca) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto need_uint = [&](unsigned& dst) -> bool {
            if (i + 1 >= argc || !parse_uint(argv[i + 1], dst)) {
                print_usage();
                return false;
            }
            ++i;
            return true;
        };
        if (a == "--N" || a == "--K") {
            if (!need_uint(opts.N)) {
                return 2;
            }
            ca.have_N = true;
        } else if (a == "--block") {
            collatz::u128 v = 0;
            if (i + 1 >= argc || !collatz::from_string(argv[i + 1], v)) {
                std::fprintf(stderr, "error: --block needs a decimal integer below 2^128\n");
                return 2;
            }
            ++i;
            opts.block_h = v;
        } else if (a == "--A") {
            if (!need_uint(opts.A)) {
                return 2;
            }
        } else if (a == "--B") {
            if (!need_uint(opts.B)) {
                return 2;
            }
            ca.have_B = true;
        } else if (a == "--bitvectors") {
            if (!need_uint(opts.num_bitvectors)) {
                return 2;
            }
        } else if (a == "--threads") {
            if (!need_uint(opts.threads)) {
                return 2;
            }
        } else if (a == "--mod3t") {
            if (!need_uint(opts.mod3t)) {
                return 2;
            }
        } else if (a == "--no-partial-oee") {
            opts.partial_oee = false;
        } else if (a == "--window-chains") {
            if (!need_uint(opts.window_chains)) {
                return 2;
            }
        } else if (a == "--window-templates-D") {
            if (!need_uint(opts.window_templates_D)) {
                return 2;
            }
        } else if (a == "--audit-cache-only") {
            ca.cache_only = true;
            ca.audit_only = true;
        } else if (a == "--help" || a == "-h") {
            ca.help = true;
            return 0;
        } else if (a == "--audit-seed" || a == "--result" || a == "--json") {
            if (i + 1 >= argc) {
                std::fprintf(
                    stderr, "error: %.*s needs a value\n", static_cast<int>(a.size()), a.data());
                return 2;
            }
            const std::string v = argv[++i];
            // An empty path (an unset variable in a script, say) would read or
            // write nothing.
            if (v.empty() && a != "--audit-seed") {
                std::fprintf(stderr,
                             "error: %.*s needs a file path\n",
                             static_cast<int>(a.size()),
                             a.data());
                return 2;
            }
            if (a == "--audit-seed") {
                ca.audit.seed = v;
                ca.have_seed = true;
            } else if (a == "--result") {
                ca.result_path = v;
                ca.audit_only = true;
            } else {
                ca.json_path = v;
                ca.audit_only = true;
            }
        } else {
            std::fprintf(
                stderr, "error: unknown argument '%.*s'\n", static_cast<int>(a.size()), a.data());
            print_usage();
            return 2;
        }
    }
    return 0;
}

// Returns 0, or 2 after printing the error.
int check_certify_args(collatz::certify::Options& opts, const CertifyArgs& ca) {
    if (!ca.have_N || opts.N < 2 || opts.N > 64 || opts.A >= opts.N) {
        std::fprintf(stderr, "error: --N must be given with A < N <= 64\n");
        print_usage();
        return 2;
    }
    if (opts.mod3t > collatz::preimage::max_t) {
        std::fprintf(stderr, "error: --mod3t must be in [0, %u]\n", collatz::preimage::max_t);
        return 2;
    }
    if (opts.window_chains > collatz::preimage::max_t) {
        std::fprintf(
            stderr, "error: --window-chains must be in [0, %u]\n", collatz::preimage::max_t);
        return 2;
    }
    if (opts.window_templates_D > collatz::preimage::max_window_t) {
        std::fprintf(stderr,
                     "error: --window-templates-D must be in [0, %u]\n",
                     collatz::preimage::max_window_t);
        return 2;
    }
    if (opts.num_bitvectors > collatz::verify::kMaxBitvectors) {
        std::fprintf(
            stderr, "error: --bitvectors must be at most %u\n", collatz::verify::kMaxBitvectors);
        return 2;
    }
    if (opts.threads > collatz::verify::max_threads()) {
        std::fprintf(stderr,
                     "error: --threads must be at most %u here (4 per hardware thread, at least "
                     "256; 0 = all hardware threads)\n",
                     collatz::verify::max_threads());
        return 2;
    }
    if (!ca.have_seed && ca.audit_only) {
        std::fprintf(stderr,
                     "error: --result, --json and --audit-cache-only go with --audit-seed\n");
        return 2;
    }
    return 0;
}

void apply_certify_defaults(collatz::certify::Options& opts, const CertifyArgs& ca) {
    // Without --B, B is the CPU verifier's default (verify::auto_step2_B: 24
    // below N=40, 30 at N=40..43, 31 from N=44), so `certify --N 40` audits the
    // B=30 depth that verify uses.
    if (!ca.have_B) {
        opts.B = collatz::verify::auto_step2_B(opts.N, /*use_gpu=*/false);
    }
    if (ca.have_seed) {
        // An audit replays every exclusion of its sample, so no sampling
        // default applies, and it loads the verifier's cached tables.
        opts.bv_cache = true;
        opts.cache_only = ca.cache_only;
        return;
    }
    // Default sampling: every window exclusion below N=38, 1 in 4 from N=38,
    // where a full replay takes too long.
    if (opts.N >= 38u) {
        opts.replay_sample = 4;
    }
}

} // namespace collatz::cli::cmd_certify
