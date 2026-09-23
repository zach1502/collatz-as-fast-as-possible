#include "cmd_certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/cli.hpp"
#include "collatz/u128.hpp"

#include <cstdio>
#include <exception>

//   collatz certify --N K [--A 10 --B auto --bitvectors 8 --threads T --mod3t 6]
//                   [--no-partial-oee] [--block H]
//   collatz certify --block H --K K --audit-seed S [--result FILE] [--json FILE]
//                   [--audit-cache-only] [--A ... --threads T]
//
// Full certificate audit: for every exclusion the verifier makes (Step-1
// descent/path-merge/OEE/mod-3^t class prunes, Step-2 mod-9/f<fmin/bitvector
// extension prunes), certify recomputes a descent-or-merge certificate to a
// strictly smaller integer. mod-9 and f<fmin are always fully certified. The
// Theorem-4.4 window replay is costly, so from N=38 it replays 1 in 4 of those
// prunes (every one below N=38). A failing certificate is a soundness bug.
// --block H audits the frontier block [H*2^K, (H+1)*2^K) as `verify --block`
// runs it, on the concrete lifted block members (collatz/certify.hpp).
//
// --audit-seed S runs the random audit of a block instead (certify::audit):
// the same checks on the residues the seed draws, with the Step-2 tables from
// the verifier's cache. --result names the run's `verify --json` file
// (cmd_certify_result.cpp): it is refused (exit 2) unless its result_digest is
// the digest of its fields and its parameters match; its argmax, record list
// and counters are recomputed (certify::ReportedRun), and the sampled
// candidates' walks are checked against its max_excursion and record list. The
// audit is printed as JSON (and written to --json FILE), with the recomputed
// digest; exit 1 when a check fails.

namespace collatz::cli::cmd_certify {

void print_usage() {
    std::fprintf(stderr,
                 "usage: collatz certify --N K [--A 10 --B auto --bitvectors 8 --threads T]\n"
                 "                       [--mod3t 6] [--no-partial-oee] [--block H]\n"
                 "       collatz certify --block H --K K --audit-seed S [--result FILE]\n"
                 "                       [--json FILE] [--audit-cache-only] ...\n"
                 "  --N K           required; audit the exclusions for n < 2^K (A < K <= 64):\n"
                 "                  every one below K=38; from K=38 the window exclusions are\n"
                 "                  sampled 1 in 4 (the verdict says so), the others all checked\n"
                 "  --K K           alias of --N (the block depth)\n"
                 "  --block H       audit the frontier block [H*2^K, (H+1)*2^K) on its own\n"
                 "                  numbers (the lifted survivors, as `verify --block` runs it)\n"
                 "  --A a           Step-2 high bits (default 10)\n"
                 "  --B b           Step-2 look-ahead width (default: shipped CPU auto-B,\n"
                 "                  24 at N<40, 30 at 40..43, 31 at N>=44)\n"
                 "  --bitvectors i  Step-2 bitvector count (default 8; at most 32)\n"
                 "  --threads T     worker threads (default 1; 0 = hardware; at most 4 per\n"
                 "                  hardware thread or 256, whichever is larger)\n"
                 "  --mod3t T       mod-3^t Step-1 sieve (default 6; 0=off)\n"
                 "  --window-chains T  Step-2 window chain-merge dip terms (default 6; 0=off)\n"
                 "  --window-templates-D D  general merge-template dip terms (default 12; 0=off)\n"
                 "  --no-partial-oee  disable high-bit partial OEE (on by default)\n"
                 "  --audit-seed S  random audit of the block: check the residues seed S draws\n"
                 "                  (any text; the worker uses the unit's result_digest)\n"
                 "  --result FILE   the block's `verify --json` file: its result_digest must be\n"
                 "                  the digest of its fields and its parameters must match; its\n"
                 "                  argmax, records and counters are recomputed, and walked\n"
                 "                  candidates are checked against its maximum and record list\n"
                 "  --json FILE     also write the audit's JSON to FILE\n"
                 "  --audit-cache-only  fail when the Step-2 tables are not in the cache instead\n"
                 "                  of building them (2^(B+1) bytes for the dip; the worker\n"
                 "                  passes it)\n"
                 "  --help, -h      print this text\n");
}

} // namespace collatz::cli::cmd_certify

namespace {

using namespace collatz::cli::cmd_certify;

void print_report(const collatz::certify::Options& opts, const collatz::certify::Report& rep) {
    std::printf("certify: N=%u A=%u B=%u bitvectors=%u threads=%u mod3t=%u partial_oee=%s "
                "window_chains=%u window_templates_D=%u\n",
                opts.N,
                opts.A,
                opts.B,
                opts.num_bitvectors,
                opts.threads,
                opts.mod3t,
                opts.partial_oee ? "on" : "off",
                opts.window_chains,
                opts.window_templates_D);
    std::printf("  M=%u fmin=%u  sample=1-in-%u\n", rep.M, rep.fmin, opts.replay_sample);
    if (rep.block_h != 0) {
        const collatz::u128 end = rep.L + (static_cast<collatz::u128>(1) << opts.N);
        std::printf("  block: H=%s K=%u L=%s end=%s  (every exclusion re-derived on the block's "
                    "own numbers)\n",
                    collatz::to_string(rep.block_h).c_str(),
                    opts.N,
                    collatz::to_string(rep.L).c_str(),
                    collatz::to_string(end).c_str());
    }
    std::printf("-- Phase 1: Step-1 class exclusions (all fully certified) --\n");
    std::printf("  descent      = %llu\n", static_cast<unsigned long long>(rep.s1_descent));
    std::printf("  path_merge   = %llu\n", static_cast<unsigned long long>(rep.s1_path_merge));
    std::printf("  oee          = %llu\n", static_cast<unsigned long long>(rep.s1_oee));
    std::printf("  mod3t        = %llu\n", static_cast<unsigned long long>(rep.s1_mod3t));
    if (rep.block_h != 0) {
        std::printf("  block ends   = %llu  (classes re-derived at both ends inside the block)\n",
                    static_cast<unsigned long long>(rep.s1_block_checked));
    }
    std::printf("-- Phase 2: Step-2 extension exclusions --\n");
    std::printf("  survivors    = %llu\n", static_cast<unsigned long long>(rep.s2_survivors));
    std::printf("  checked      = %llu\n",
                static_cast<unsigned long long>(rep.s2_candidates_checked));
    std::printf("  excluded     = %llu\n", static_cast<unsigned long long>(rep.s2_excluded_total));
    std::printf("    mod9       = %llu  (fully certified)\n",
                static_cast<unsigned long long>(rep.s2_mod9));
    std::printf("    f<fmin     = %llu  (fully certified)\n",
                static_cast<unsigned long long>(rep.s2_f_lt_fmin));
    std::printf("    window     = %llu  (replayed %llu, %.4f%% coverage)\n",
                static_cast<unsigned long long>(rep.s2_window_total),
                static_cast<unsigned long long>(rep.s2_window_replayed),
                rep.replay_coverage() * 100.0);
    std::printf("      within-M = %llu  (partial-OEE/descent at k<=M)\n",
                static_cast<unsigned long long>(rep.s2_window_within_m));
    std::printf("      in-window= %llu  (bitvector at (M, M+B])\n",
                static_cast<unsigned long long>(rep.s2_window_in_window));
    std::printf("      chain    = %llu  (backward chain-merge, y<n T^L(y)=T^s(n))\n",
                static_cast<unsigned long long>(rep.s2_window_chain));
    std::printf("-- Result --\n");
    std::printf("  failures     = %llu\n", static_cast<unsigned long long>(rep.failures_total));
    for (const collatz::certify::Failure& f : rep.failures) {
        std::printf(
            "    FAIL n0=%s n=%s k=%u  %s\n", f.n0.c_str(), f.n.c_str(), f.k, f.reason.c_str());
    }
    std::printf("  seconds      = %.3f\n", rep.seconds);
    if (!rep.clean()) {
        std::printf("  VERDICT      = BUG\n");
    } else if (rep.s2_window_replayed < rep.s2_window_total) {
        // From N=38 only 1 in replay_sample window exclusions were replayed.
        std::printf("  VERDICT      = CLEAN (every checked certificate holds; sampled 1 in %u "
                    "window exclusions: %llu of %llu replayed)\n",
                    opts.replay_sample,
                    static_cast<unsigned long long>(rep.s2_window_replayed),
                    static_cast<unsigned long long>(rep.s2_window_total));
    } else {
        std::printf("  VERDICT      = CLEAN (all certificates hold)\n");
    }
}

int run_certify(int argc, char** argv) {
    collatz::certify::Options opts;
    opts.window_templates_D = 12; // verify's default: the replay searches
                                  // ancestors to depth max(2*t_w, D) to witness template certs.
    CertifyArgs ca;
    if (const int rc = parse_certify_args(argc, argv, opts, ca); rc != 0) {
        return rc;
    }
    if (ca.help) {
        print_usage();
        return 0;
    }
    if (const int rc = check_certify_args(opts, ca); rc != 0) {
        return rc;
    }
    apply_certify_defaults(opts, ca);
    if (ca.have_seed) {
        return run_audit(opts, ca.audit, ca.result_path, ca.json_path);
    }

    collatz::certify::Report rep;
    try {
        rep = collatz::certify::run(opts);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }

    print_report(opts, rep);
    return rep.clean() ? 0 : 1;
}

} // namespace

COLLATZ_REGISTER_COMMAND("certify", "full descent-or-merge certificate audit", run_certify)
