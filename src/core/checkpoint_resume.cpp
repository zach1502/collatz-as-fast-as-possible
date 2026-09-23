// The slicing of a checkpointed run: adopted from DIR/run.json on a resume,
// the default slice count for a new run, and the checks of both.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace collatz::checkpoint {

namespace fs = std::filesystem;

std::uint64_t max_slices(unsigned N, unsigned A) noexcept {
    static_assert(kMaxSlices == (std::uint64_t{1} << 10), "max_slices caps K at kMaxSlices");
    const unsigned M = A < N ? N - A : N;
    if (M < 2) {
        return 1;
    }
    const unsigned bits = std::min(M - 2u, 10u);
    return std::uint64_t{1} << bits;
}

std::uint64_t default_slices(unsigned N, unsigned A, bool use_gpu) noexcept {
    const unsigned base = use_gpu ? 52u : 46u;
    const unsigned e = N > base ? std::min(N - base, 10u) : 0u;
    return std::min(std::uint64_t{1} << e, max_slices(N, A));
}

namespace detail {

// Resume: DIR/run.json must describe this run; its slice count and range are
// adopted.
bool adopt_run_json(RunConfig& cfg, const fs::path& run_path, std::string& err) {
    std::string text;
    std::string perr;
    std::optional<Json> rj;
    if (read_file(run_path.string(), text, perr)) {
        rj = parse_json(text, &perr);
    }
    if (!rj || rj->type != Json::Type::Object) {
        err = "'" + run_path.string() + "' is unreadable (" + perr +
              "); rerun with --checkpoint-force to start over";
        return false;
    }
    Fingerprint stored;
    if (const Json* fpj = rj->get("fingerprint");
        fpj != nullptr && fpj->type == Json::Type::Object) {
        for (const auto& [k, v] : fpj->obj) {
            stored[k] = v.text;
        }
    }
    const std::vector<std::string> diff = fingerprint_diff(stored, cfg.fingerprint);
    if (!diff.empty()) {
        err = "checkpoint directory '" + cfg.dir + "' belongs to a run with different parameters:";
        for (const auto& d : diff) {
            err += "\n    " + d;
        }
        err += "\n  Resume with the original options, use another --checkpoint DIR, or add "
               "--checkpoint-force to discard that run and start over.";
        return false;
    }
    std::uint64_t sK = 0;
    std::uint64_t slo = 0;
    std::uint64_t shi = 0;
    std::string e2;
    if (!get_u64(&*rj, "slices", sK, e2) || !get_u64(&*rj, "slice_lo", slo, e2) ||
        !get_u64(&*rj, "slice_hi", shi, e2)) {
        err = "'" + run_path.string() + "': " + e2 + "; rerun with --checkpoint-force";
        return false;
    }
    // Read back from disk, so bounded before the slice scan uses them
    // (slice_hi 0 stands for all slices, as in RunConfig).
    const std::uint64_t shi_or_k = shi == 0 ? sK : shi;
    if (sK == 0 || sK > kMaxSlices || (sK & (sK - 1)) != 0 || slo >= shi_or_k || shi_or_k > sK) {
        err = "'" + run_path.string() + "' names " + std::to_string(sK) + " slices and range " +
              std::to_string(slo) + ":" + std::to_string(shi) +
              "; a run has a power of two in [1, " + std::to_string(kMaxSlices) +
              "] slices and 0 <= lo < hi <= slices. Rerun with --checkpoint-force";
        return false;
    }
    if (cfg.K_explicit && cfg.K != sK) {
        err = "checkpoint directory '" + cfg.dir + "' was started with --slices " +
              std::to_string(sK) + " (this run asks for " + std::to_string(cfg.K) +
              "); drop --slices to resume, or add --checkpoint-force to start over";
        return false;
    }
    if (cfg.range_explicit && (cfg.lo != slo || (cfg.hi == 0 ? sK : cfg.hi) != shi)) {
        err = "checkpoint directory '" + cfg.dir + "' was started with --slice-range " +
              std::to_string(slo) + ":" + std::to_string(shi) +
              "; use the same range, another DIR, or --checkpoint-force";
        return false;
    }
    cfg.K = sK;
    cfg.lo = slo;
    cfg.hi = shi;
    std::fprintf(stderr,
                 "checkpoint: resuming %s (%llu slices, parameters match)\n",
                 cfg.dir.c_str(),
                 static_cast<unsigned long long>(cfg.K));
    // The commit is the only guard against resuming with a different
    // binary. Warn (do not refuse) when it cannot vouch for that.
    const auto cit = cfg.fingerprint.find("commit");
    const std::string commit = cit == cfg.fingerprint.end() ? "unknown" : cit->second;
    if (commit == "unknown") {
        std::fprintf(stderr,
                     "checkpoint: warning: this binary's commit is unknown (built outside a "
                     "git checkout?), so the resume cannot check that the finished slices "
                     "came from the same code\n");
    } else if (commit.size() > 6 && commit.compare(commit.size() - 6, 6, "-dirty") == 0) {
        std::fprintf(stderr,
                     "checkpoint: warning: built from a modified tree (%s); slices from a "
                     "different uncommitted change would carry the same commit\n",
                     commit.c_str());
    }
    return true;
}

// A new run: the default slice count unless --slices was given.
void init_new_run(RunConfig& cfg) {
    if (!cfg.K_explicit) {
        cfg.K = default_slices(cfg.opts.N, cfg.opts.A, cfg.meta.use_gpu);
        std::fprintf(stderr,
                     "checkpoint: --slices not given: using %llu slice(s) for N=%u%s "
                     "(about a few minutes each; override with --slices K)\n",
                     static_cast<unsigned long long>(cfg.K),
                     cfg.opts.N,
                     cfg.meta.use_gpu ? " on the GPU" : " on the CPU");
    }
    if (cfg.hi == 0) {
        cfg.hi = cfg.K;
    }
}

bool check_slicing(const RunConfig& cfg, std::string& err) {
    if (cfg.K == 0 || (cfg.K & (cfg.K - 1)) != 0) {
        err = "--slices must be a power of two";
        return false;
    }
    const std::uint64_t kmax = max_slices(cfg.opts.N, cfg.opts.A);
    if (cfg.K > kmax) {
        err = "--slices " + std::to_string(cfg.K) +
              " is too fine for N=" + std::to_string(cfg.opts.N) +
              ", A=" + std::to_string(cfg.opts.A) + " (max " + std::to_string(kmax) + ")";
        return false;
    }
    if (cfg.lo >= cfg.hi || cfg.hi > cfg.K) {
        err = "--slice-range a:b needs 0 <= a < b <= K (K = " + std::to_string(cfg.K) + ")";
        return false;
    }
    return true;
}

} // namespace detail

} // namespace collatz::checkpoint
