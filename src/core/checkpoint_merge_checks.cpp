// The checks that a slice set is complete and comes from one run: coverage,
// check_slice_set and check_slices_match.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace collatz::checkpoint {

void coverage(const std::vector<Slice>& slices,
              std::uint64_t K,
              std::vector<std::uint64_t>& missing,
              std::vector<std::uint64_t>& duplicate) {
    missing.clear();
    duplicate.clear();
    // `seen` and `missing` are sized by K, which can come from a slice file.
    if (K == 0 || K > kMaxSlices) {
        throw std::invalid_argument("checkpoint::coverage: slice count " + std::to_string(K) +
                                    " is outside [1, " + std::to_string(kMaxSlices) + "]");
    }
    std::vector<unsigned> seen(static_cast<std::size_t>(K), 0u);
    for (const Slice& s : slices) {
        if (s.k == K && s.c < K) {
            ++seen[static_cast<std::size_t>(s.c)];
        }
    }
    for (std::uint64_t c = 0; c < K; ++c) {
        if (seen[static_cast<std::size_t>(c)] == 0) {
            missing.push_back(c);
        } else if (seen[static_cast<std::size_t>(c)] > 1) {
            duplicate.push_back(c);
        }
    }
}

namespace {

bool same_preflight(const verify::Result& a, const verify::Result& b) {
    return a.preflight_candidates == b.preflight_candidates &&
           a.preflight_coverage == b.preflight_coverage && a.preflight_pow == b.preflight_pow &&
           a.preflight_blocks == b.preflight_blocks &&
           a.preflight_step3_steps == b.preflight_step3_steps;
}

} // namespace

namespace detail {

std::vector<std::string> run_params_diff(const Slice& a, const Slice& b) {
    std::vector<std::string> d;
    const auto cmp = [&d](const char* key, const std::string& x, const std::string& y) {
        if (x != y) {
            d.push_back(std::string(key) + ": " + x + " vs " + y);
        }
    };
    const auto U = [](std::uint64_t v) { return std::to_string(v); };
    const auto Q = [](const std::string& t) { return "\"" + t + "\""; };
    const auto thr = [&Q](const verify::Options& o) {
        return o.record_threshold.empty() ? std::string("null") : Q(o.record_threshold);
    };
    const verify::Options& x = a.opts;
    const verify::Options& y = b.opts;
    cmp("N", U(a.N), U(b.N));
    cmp("block_h", collatz::to_string(a.block_h), collatz::to_string(b.block_h));
    cmp("A", U(x.A), U(y.A));
    cmp("B", U(x.B), U(y.B));
    cmp("bitvectors", U(x.num_bitvectors), U(y.num_bitvectors));
    cmp("mod3t", U(x.step1.mod3t), U(y.step1.mod3t));
    cmp("partial_oee", x.partial_oee ? "true" : "false", y.partial_oee ? "true" : "false");
    cmp("window_chains", U(x.window_chains), U(y.window_chains));
    cmp("window_templates_D", U(x.window_templates_D), U(y.window_templates_D));
    cmp("record_threshold", thr(x), thr(y));
    cmp("assumption_source", Q(verify::assumption_source(x)), Q(verify::assumption_source(y)));
    cmp("unwalked_depth", U(a.unwalked_depth), U(b.unwalked_depth));
    cmp("commit", Q(a.commit), Q(b.commit));
    return d;
}

// Every slice has the same count K, a power of two in [1, kMaxSlices], and
// together they cover 0..K-1 once.
bool check_slice_set(const std::vector<Slice>& slices, std::uint64_t K, std::string& err) {
    std::vector<std::uint64_t> missing;
    std::vector<std::uint64_t> dup;
    for (const Slice& s : slices) {
        if (s.k != K) {
            err = "slices come from different slice counts (case_k " + std::to_string(K) + " vs " +
                  std::to_string(s.k) + " in " + s.source + ")";
            return false;
        }
    }
    // coverage() would throw on a K outside [1, kMaxSlices]; report it instead.
    if (K == 0 || K > kMaxSlices || (K & (K - 1)) != 0) {
        err = "slice count (case_k) " + std::to_string(K) + " is not a power of two in [1, " +
              std::to_string(kMaxSlices) + "]";
        return false;
    }
    coverage(slices, K, missing, dup);
    if (!dup.empty() || !missing.empty() || slices.size() != K) {
        std::string m = "slice set does not cover 0.." + std::to_string(K - 1) + " exactly once";
        if (!dup.empty()) {
            m += "; duplicated:";
            for (const auto c : dup) {
                m += " " + std::to_string(c);
            }
        }
        if (!missing.empty()) {
            m += "; missing (" + std::to_string(missing.size()) + "):";
            std::size_t shown = 0;
            for (const auto c : missing) {
                if (++shown > 64) {
                    m += " ...";
                    break;
                }
                m += " " + std::to_string(c);
            }
        }
        err = m;
        return false;
    }
    return true;
}

// The slices come from one run: same N, block, preflight, kernel and options.
bool check_slices_match(const std::vector<Slice>& slices, std::string& err) {
    const Slice& s0 = slices[0];
    for (const Slice& s : slices) {
        if (s.N != s0.N) {
            err = "slices have different N (" + s0.source + " vs " + s.source + ")";
            return false;
        }
        if (s.block_h != s0.block_h) {
            err = "slices come from different blocks (block_h " + collatz::to_string(s0.block_h) +
                  " in " + s0.source + " vs " + collatz::to_string(s.block_h) + " in " + s.source +
                  ")";
            return false;
        }
        if (!same_preflight(s.r, s0.r)) {
            err = "preflight blocks differ across slices (" + s0.source + " vs " + s.source +
                  "); slices must share N, kernel and backend";
            return false;
        }
        if (s.r.pow_available != s0.r.pow_available || s.r.pow_kernel_id != s0.r.pow_kernel_id) {
            err = "pow_available/pow_kernel_id differ across slices";
            return false;
        }
        if (s.params != s0.params) {
            // Without a fingerprint this is the only parameter check, so it
            // must catch A/B/kernel/unwalked_depth/sieve-toggle mismatches.
            std::string m = "run parameters (params block) differ between " + s0.source + " and " +
                            s.source + ":";
            std::set<std::string> keys;
            for (const auto& kv : s0.params) {
                keys.insert(kv.first);
            }
            for (const auto& kv : s.params) {
                keys.insert(kv.first);
            }
            for (const auto& k : keys) {
                const auto a = s0.params.find(k);
                const auto b = s.params.find(k);
                const std::string av = a == s0.params.end() ? "<absent>" : a->second;
                const std::string bv = b == s.params.end() ? "<absent>" : b->second;
                if (av != bv) {
                    m += "\n    " + k + ": " + av + " vs " + bv;
                }
            }
            m += "\n  Only slices of one run (same N, A, B, kernel and options) can be merged.";
            err = m;
            return false;
        }
        if (s.unwalked_depth != s0.unwalked_depth) {
            err = "unwalked_depth differs between " + s0.source + " and " + s.source +
                  " (slices of one run share it)";
            return false;
        }
        // The same comparison as the resume scan's, which also covers the
        // commit: the merged JSON records the slices' commit.
        if (const std::vector<std::string> d = run_params_diff(s0, s); !d.empty()) {
            std::string m = "run parameters differ between " + s0.source + " and " + s.source + ":";
            for (const std::string& line : d) {
                m += "\n    " + line;
            }
            m += "\n  Only slices of one run (same options, written by one commit) can be merged.";
            err = m;
            return false;
        }
        if (s.fingerprint != s0.fingerprint) {
            std::string m = "run parameters differ between " + s0.source + " and " + s.source;
            for (const auto& d : fingerprint_diff(s0.fingerprint, s.fingerprint)) {
                m += "\n    " + d;
            }
            err = m;
            return false;
        }
    }
    return true;
}

} // namespace detail

} // namespace collatz::checkpoint
