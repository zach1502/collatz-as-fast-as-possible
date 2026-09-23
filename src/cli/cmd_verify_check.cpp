#include "cmd_verify_internal.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step2/params.hpp"
#include "collatz/threading.hpp"
#include "collatz/verify.hpp"

#include <cstdio>

// Checks of the parsed verify options: the range and block options, the
// checkpoint options, the sieve bounds and the GPU host routing.

namespace collatz::cli::verify_detail {

namespace {

// --N, the block options and the record-threshold default.
int check_range_args(collatz::verify::Options& opts, VerifyArgs& va) {
    if (!va.have_N || opts.N < 2 || opts.N > 96) {
        std::fprintf(stderr, "error: --N must be given and in [2, 96]\n");
        print_usage();
        return 2;
    }
    if (va.have_block && opts.N < 32u) {
        std::fprintf(stderr, "error: block mode needs --K in [32, 76+A] (got K=%u)\n", opts.N);
        return 2;
    }
    // A block that starts at or above 2^71 lists by default its walked numbers
    // whose peak exceeds that of Barina's path record #97
    // (default_record_threshold).
    if (opts.record_threshold.empty() && !va.record_threshold_off) {
        opts.record_threshold = collatz::verify::default_record_threshold(opts);
        if (!opts.record_threshold.empty()) {
            std::fprintf(stderr,
                         "note: --record-threshold defaults to %s, the T-map peak of Barina's "
                         "path record #97 (n = %s), for a block starting at or above 2^71 "
                         "(--record-threshold none turns the list off)\n",
                         opts.record_threshold.c_str(),
                         collatz::verify::kBarinaRecord97N);
        }
    }
    return 0;
}

int check_checkpoint_args(const VerifyArgs& va) {
    if (va.checkpoint_dir.empty() && (va.ck_slices != 0 || va.ck_range || va.ck_force)) {
        std::fprintf(
            stderr, "error: --slices / --slice-range / --checkpoint-force need --checkpoint DIR\n");
        return 2;
    }
    if (!va.checkpoint_dir.empty()) {
        if (va.ck_range && va.ck_slices == 0) {
            std::fprintf(stderr,
                         "error: --slice-range needs an explicit --slices K (every machine must "
                         "use the same K)\n");
            return 2;
        }
        if (va.ck_range && va.ck_hi > va.ck_slices) {
            std::fprintf(stderr, "error: --slice-range b must be <= --slices K\n");
            return 2;
        }
    }
    return 0;
}

// Window-term depths and the u128 DFS bound.
int check_sieve_args(const collatz::verify::Options& opts) {
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
    // The pipeline is exact only while the Step-1 DFS value T^k(n0) < 3^M fits
    // in u128, that is M = N - A <= 76, with headroom for the pmax bound. The
    // candidate n, the prefix values, m_a and the Step-3 excursions are u128
    // with GMP fallbacks past 2^128, so starting values with N > 64 work up to
    // this bound.
    {
        const unsigned M_dfs = opts.A < opts.N ? opts.N - opts.A : opts.N;
        if (M_dfs > 76) {
            // M_dfs <= N, so N > 76 here. The smallest A that meets the bound is
            // N - 76; suggest it only when Step 2 accepts it (A <= max_A).
            const unsigned need_A = opts.N - 76u;
            if (need_A <= collatz::step2::max_A) {
                std::fprintf(stderr,
                             "error: N - A = %u exceeds the u128 DFS bound of 76; raise --A "
                             "to %u..%u (e.g. --A %u) or lower --N to at most %u\n",
                             M_dfs,
                             need_A,
                             collatz::step2::max_A,
                             need_A,
                             opts.A + 76u);
            } else {
                std::fprintf(stderr,
                             "error: N - A = %u exceeds the u128 DFS bound of 76, and --A is "
                             "at most %u; lower --N to at most %u (with --A %u)\n",
                             M_dfs,
                             collatz::step2::max_A,
                             76u + collatz::step2::max_A,
                             collatz::step2::max_A);
            }
            return 2;
        }
    }
    return 0;
}

void route_gpu_host(collatz::verify::Options& opts, VerifyArgs& va) {
    // The GPU tail paths need a Step-1 depth N <= 64 (a block's values may be
    // wider, see value_bits), so N > 64 runs on the exact u128 CPU pipeline.
    if (va.use_gpu && opts.N > 64u) {
        std::fprintf(stderr, "note: --gpu unsupported for N > 64; using the CPU pipeline\n");
        va.use_gpu = false;
    }
    // With --gpu and no --threads, the host uses every hardware thread for the
    // Step-1 producers.
    if (va.use_gpu && !va.have_threads) {
        opts.threads = collatz::hardware_threads();
        std::fprintf(stderr,
                     "note: --gpu without --threads: defaulting to %u host threads "
                     "(hardware_threads; producers run Step 1 on the CPU)\n",
                     opts.threads);
    }
}

} // namespace

// Returns 0, or 2 after printing the error.
int check_args(collatz::verify::Options& opts, VerifyArgs& va) {
    if (const int rc = check_range_args(opts, va); rc != 0) {
        return rc;
    }
    if (const int rc = check_checkpoint_args(va); rc != 0) {
        return rc;
    }
    if (const int rc = check_sieve_args(opts); rc != 0) {
        return rc;
    }
    route_gpu_host(opts, va);
    if (opts.step1.mod3t > collatz::preimage::max_t) {
        std::fprintf(stderr, "error: --mod3t must be in [0, %u]\n", collatz::preimage::max_t);
        return 2;
    }
    return 0;
}

} // namespace collatz::cli::verify_detail
