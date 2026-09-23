#include "cmd_verify_internal.hpp"
#include "collatz/verify.hpp"

#include <cstdio>
#include <cstdlib>

// The option defaults of the verify command: the values set before parsing and
// the A and B defaults that depend on N and on the backend.

namespace collatz::cli::verify_detail {

void set_cli_defaults(collatz::verify::Options& opts) {
    opts.A = 10;
    opts.partial_oee = true;
    opts.step1.mod3t = 6;
    // The window terms are sound merge certificates folded into the precomputed
    // dip table, so they add no cost per survivor and keep the maximum exact.
    opts.window_chains = 6;
    opts.window_templates_D = 12;
}

void apply_auto_a(collatz::verify::Options& opts, const VerifyArgs& va) {
    // On the CPU, A defaults to 9 from N=44 and to 10 below. With the large
    // cached bitvectors every survivor's window read misses the cache, so the
    // saving from fewer survivors (a higher A) shrinks while the candidate
    // count still grows with A. On the GPU, A defaults to 10 below N=48 and
    // to 12 from N=48.
    if (!va.have_A && !va.use_gpu && opts.N >= 44u) {
        opts.A = 9u;
    }
    if (!va.have_A && va.use_gpu && opts.N >= 48u) {
        opts.A = 12u;
        std::fprintf(stderr, "note: GPU at N>=48: defaulting to --A 12\n");
    }
}

void apply_auto_b(collatz::verify::Options& opts, const VerifyArgs& va) {
    // Without --B, B comes from auto_step2_B in collatz/verify/defaults.hpp,
    // which test_core_verify_defaults pins. The bitvectors are cached on disk,
    // keyed by B, N-A and the bitvector count, so a warm run loads a large-B
    // filter, and the instruction count falls as B grows. A larger B costs memory
    // (8 bitvectors of 2^B bits) and a longer load, which a short run does not
    // recover, so B is 24 below N=40. From N=44 B is 31, because B=32 needs about
    // 8 GiB resident, over the 8 GiB budget.
    if (!va.have_B) {
        opts.B = collatz::verify::auto_step2_B(opts.N, va.use_gpu);
        // The GPU uses B=32 from N=48 (with A=12). The deeper look-ahead walks
        // fewer candidates, so the counters and digests differ from a B=31 run,
        // while the maximum and its argmax do not. It needs a 4 GiB table cache
        // per N; the dip cache, 8 GiB at B=32, is off by default here. On
        // smaller cards the device memory budget lowers the candidate cap, and
        // the runner falls back to the CPU if the table does not fit.
        if (va.use_gpu && opts.N >= 48u && opts.A == 12u && opts.B == 31u) {
            opts.B = 32u;
            if (std::getenv("COLLATZ_DIP_CACHE") == nullptr) {
                _putenv_s("COLLATZ_DIP_CACHE", "0");
            }
            std::fprintf(stderr, "note: GPU at N>=48: defaulting to --B 32\n");
        }
    }
}

} // namespace collatz::cli::verify_detail
