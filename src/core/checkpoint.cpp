// The run fingerprint and the list of COLLATZ_* environment variables that can
// change a Result. See collatz/checkpoint.hpp for the design.

#include "collatz/checkpoint.hpp"

#include <cstdlib>

namespace collatz::checkpoint {

namespace {

// Environment variables that can change a printed Result scalar. They are part
// of the fingerprint, so a run cannot be resumed with another value. The rule
// (see result_env_names() in the header): cross-checks, fault injection, any
// variable whose effect is in doubt, and every variable that selects another
// Step-2 build, Step-2 mask, Step-3 tail algorithm or kernel variant go here,
// even where the variant is designed and tested to be bit-identical, because a
// defect in a variant would change the result. A false entry costs only that
// the variable cannot change between pause and resume.
const std::vector<const char*> kResultEnv = {
    // Fault injection.
    "COLLATZ_GPU_SELFCHECK_INJECT",
    "COLLATZ_GPU_SELFCHECK_INJECT_COPY",
    "COLLATZ_GPU_SELFCHECK_INJECT_DROPWG",
    "COLLATZ_GPU_SELFCHECK_INJECT_EXPAND",
    "COLLATZ_GPU_SELFCHECK_INJECT_LOST",
    "COLLATZ_GPU_EARLY_UPLOAD_INJECT_FAIL",
    "COLLATZ_GPU_INJECT_CL_FAIL",
    "COLLATZ_GPU_INJECT_PRODUCER_FAIL",
};

} // namespace

const std::vector<const char*>& result_env_names() {
    return kResultEnv;
}

Fingerprint make_fingerprint(const verify::Options& o, bool use_gpu, const std::string& commit) {
    Fingerprint fp;
    const auto U = [](unsigned long long v) { return std::to_string(v); };
    const auto Bo = [](bool b) { return std::string(b ? "1" : "0"); };
    fp["N"] = U(o.N);
    fp["block_h"] = collatz::to_string(o.block_h);
    fp["record_threshold"] = o.record_threshold;
    // Not a Result scalar, but part of every slice's params: a resume with
    // another source would leave slices that refuse to merge.
    fp["assumption_source"] = verify::assumption_source(o);
    fp["A"] = U(o.A);
    fp["B"] = U(o.B);
    fp["bitvectors"] = U(o.num_bitvectors);
    fp["mod3t"] = U(o.step1.mod3t);
    fp["partial_oee"] = Bo(o.partial_oee);
    fp["window_chains"] = U(o.window_chains);
    fp["window_templates_D"] = U(o.window_templates_D);
    fp["gpu"] = Bo(use_gpu);
    fp["commit"] = commit;
    for (const char* name : kResultEnv) {
        const char* v = std::getenv(name);
        fp[std::string("env:") + name] = v != nullptr ? v : "";
    }
    return fp;
}

std::vector<std::string> fingerprint_diff(const Fingerprint& stored, const Fingerprint& now) {
    std::vector<std::string> out;
    for (const auto& [k, v] : now) {
        const auto it = stored.find(k);
        const std::string sv = it == stored.end() ? "<absent>" : it->second;
        if (sv != v) {
            out.push_back(k + ": '" + sv + "' (checkpoint) vs '" + v + "' (this run)");
        }
    }
    for (const auto& [k, v] : stored) {
        if (now.find(k) == now.end()) {
            out.push_back(k + ": '" + v + "' (checkpoint) vs <absent> (this run)");
        }
    }
    return out;
}

} // namespace collatz::checkpoint
