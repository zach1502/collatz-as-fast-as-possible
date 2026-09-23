#include "cmd_merge_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/cli.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// merge combines the slice JSONs of a `verify --checkpoint` run (one or more
// checkpoint directories, such as one per machine from `--slice-range`) and
// prints the merged Result. The arithmetic is exact. Gaps (listed by slice),
// overlaps and slices from runs with other parameters are refused.

namespace {

namespace ckp = collatz::checkpoint;

using namespace collatz::cli::cmd_merge;

// Formats a sorted list as ranges, such as "3, 9..15, 20".
std::string ranges(const std::vector<std::uint64_t>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size();) {
        std::size_t j = i;
        while (j + 1 < v.size() && v[j + 1] == v[j] + 1) {
            ++j;
        }
        if (!s.empty()) {
            s += ", ";
        }
        s += std::to_string(v[i]);
        if (j > i) {
            s += ".." + std::to_string(v[j]);
        }
        i = j + 1;
    }
    return s;
}

void print_merge_usage(std::FILE* out) {
    std::fprintf(out,
                 "usage: collatz merge DIR... [--json OUT]\n"
                 "  Merge the slice files of `verify --checkpoint DIR` runs (several DIRs\n"
                 "  from --slice-range on different machines are fine) and print the\n"
                 "  merged Result. --json writes it to OUT through OUT.tmp and a rename,\n"
                 "  stamped with the slices' commit.\n");
}

// Returns 0, or 2 after printing the error or the usage; -1 after printing the
// usage for -h or --help, which exits 0.
int parse_merge_args(int argc,
                     char** argv,
                     std::vector<std::string>& inputs,
                     std::string& json_out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") {
            // An empty OUT would mean "no --json" below, so a script's unset
            // variable would skip the file with exit 0.
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::fprintf(stderr, "error: --json needs a non-empty path\n");
                return 2;
            }
            json_out = argv[++i];
        } else if (a == "-h" || a == "--help") {
            print_merge_usage(stdout);
            return -1;
        } else {
            inputs.emplace_back(a);
        }
    }
    if (inputs.empty()) {
        print_merge_usage(stderr);
        return 2;
    }
    return 0;
}

// Loads the slices and checks that they form one complete set. Returns 0, or
// the exit code after printing the error (3 when slices are missing).
int load_slice_set(const std::vector<std::string>& inputs, std::vector<ckp::Slice>& slices) {
    std::string err;
    if (!ckp::load_slices(inputs, slices, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    const std::uint64_t K = slices.front().k;
    for (const ckp::Slice& s : slices) {
        if (s.k != K) {
            std::fprintf(stderr,
                         "error: mixed slice counts: %s is 1/%llu but %s is 1/%llu\n",
                         slices.front().source.c_str(),
                         static_cast<unsigned long long>(K),
                         s.source.c_str(),
                         static_cast<unsigned long long>(s.k));
            return 2;
        }
    }
    std::vector<std::uint64_t> missing;
    std::vector<std::uint64_t> dup;
    // parse_slice keeps every case_k in [1, kMaxSlices], and coverage() sizes
    // its lists by K; the check is repeated so this path can never throw.
    if (K == 0 || K > ckp::kMaxSlices) {
        std::fprintf(stderr,
                     "error: %s: slice count %llu is outside [1, %llu]\n",
                     slices.front().source.c_str(),
                     static_cast<unsigned long long>(K),
                     static_cast<unsigned long long>(ckp::kMaxSlices));
        return 2;
    }
    ckp::coverage(slices, K, missing, dup);
    if (!dup.empty()) {
        std::fprintf(stderr,
                     "error: overlapping inputs: slice(s) %s appear more than once:\n",
                     ranges(dup).c_str());
        for (const auto& s : slices) {
            for (const auto c : dup) {
                if (s.c == c) {
                    std::fprintf(stderr, "    %s\n", s.source.c_str());
                }
            }
        }
        return 2;
    }
    if (!missing.empty()) {
        std::fprintf(stderr,
                     "error: incomplete: %llu of %llu slices present; missing slice(s): %s\n"
                     "  (finish them with `collatz verify ... --checkpoint DIR --slices %llu "
                     "--slice-range a:b`, then merge again)\n",
                     static_cast<unsigned long long>(K - missing.size()),
                     static_cast<unsigned long long>(K),
                     ranges(missing).c_str(),
                     static_cast<unsigned long long>(K));
        return 3;
    }
    return 0;
}

// Rebuilds the run's Options from the first slice's params. The merged digest
// and JSON are computed from them, so they have the shape and digest of the
// --json file of an unsliced run. parse_slice read every value with its range
// check (no parameter above 2^32-1), from the same text as the digest it
// checked, so nothing is re-read or truncated here.
void rebuild_options(const ckp::Slice& first,
                     const ckp::MergeInfo& mi,
                     collatz::verify::Options& o,
                     ckp::JsonMeta& meta) {
    meta.commit = mi.commit;   // the binary that verified the slices, not this one
    meta.backend = mi.backend; // the backend recorded in the slices
    meta.unwalked_depth = mi.depth;
    meta.use_gpu = first.gpu;
    o = first.opts;
    o.step1.case_c = 0; // the merged Result is the whole run
    o.step1.case_k = 1;
}

int run_merge(int argc, char** argv) {
    std::vector<std::string> inputs;
    std::string json_out;
    if (const int rc = parse_merge_args(argc, argv, inputs, json_out); rc != 0) {
        return rc < 0 ? 0 : rc;
    }
    std::vector<ckp::Slice> slices;
    if (const int rc = load_slice_set(inputs, slices); rc != 0) {
        return rc;
    }
    const std::uint64_t K = slices.front().k;
    collatz::verify::Result r;
    ckp::MergeInfo mi;
    std::string err;
    if (!ckp::merge(slices, r, err, &mi)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    const unsigned N = slices.front().N;
    const collatz::u128 block_h = slices.front().block_h;
    std::printf("merge: %llu slices of N=%u from %zu input(s); backend %s\n",
                static_cast<unsigned long long>(K),
                N,
                inputs.size(),
                mi.backend.c_str());
    if (block_h != 0) {
        std::printf("block: H=%s K=%u\n", collatz::to_string(block_h).c_str(), N);
    }
    print_merged_counts(N, r);
    print_merged_max_exact(N, block_h, mi, r);
    std::printf("overflow_fallbacks = %llu\n",
                static_cast<unsigned long long>(r.overflow_fallbacks));
    std::printf("step1_survivors    = %llu\n", static_cast<unsigned long long>(r.step1.survivors));
    std::printf("seconds_total      = %.6f  (sum over slices)\n", r.seconds_total);

    collatz::verify::Options o;
    ckp::JsonMeta meta;
    rebuild_options(slices.front(), mi, o, meta);
    print_merged_records(o, r);
    if (block_h != 0) {
        std::printf("assumption_source  = %s\n", collatz::verify::assumption_source(o).c_str());
    }
    std::printf("result_digest      = %s\n", ckp::result_digest(o, mi.depth, r).c_str());
    if (!json_out.empty()) {
        const std::string out = ckp::result_json(
            meta,
            o,
            r,
            "\"checkpoint\": {\"schema\": \"collatz-checkpoint-merged/1\", \"merged_slices\": " +
                std::to_string(K) + "}");
        // write_file_atomic writes a temp file and renames it, so an
        // interrupted write never leaves a truncated OUT.
        std::string werr;
        if (!ckp::write_file_atomic(json_out, out, werr)) {
            std::fprintf(stderr,
                         "error: failed to write JSON to '%s' (%s)\n",
                         json_out.c_str(),
                         werr.c_str());
            return 2;
        }
    }
    return 0;
}

} // namespace

COLLATZ_REGISTER_COMMAND("merge", "merge --checkpoint slice dirs into one Result", run_merge)
