// Reading a slice file: parse_slice, read_slice_file, the params reader and
// the shared field readers. checkpoint_slice_result.cpp reads the result and
// preflight blocks.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

namespace collatz::checkpoint {

using namespace detail;

namespace detail {

bool get_u64(const Json* obj, const char* key, std::uint64_t& out, std::string& err) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    u128 x = 0;
    if (v == nullptr || v->type != Json::Type::Number || !collatz::from_string(v->text, x) ||
        x > ~std::uint64_t{0}) {
        err = std::string("missing or non-integer field '") + key + "'";
        return false;
    }
    out = static_cast<std::uint64_t>(x);
    return true;
}

bool get_bool(const Json* obj, const char* key, bool& out, std::string& err) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    if (v == nullptr || v->type != Json::Type::Bool) {
        err = std::string("missing boolean field '") + key + "'";
        return false;
    }
    out = v->b;
    return true;
}

} // namespace detail

namespace {

/// A params field that verify writes from an `unsigned` option, so a value
/// above 2^32-1 cannot come from it.
bool get_u32(const Json* obj, const char* key, unsigned& out, std::string& err) {
    std::uint64_t v = 0;
    if (!get_u64(obj, key, v, err)) {
        return false;
    }
    if (v > 0xFFFFFFFFu) {
        err = std::string("params.") + key + " is " + std::to_string(v) + ", above 2^32-1";
        return false;
    }
    out = static_cast<unsigned>(v);
    return true;
}

/// The raw params text that merge() compares across slices, without the
/// per-slice fields.
void read_params_map(const Json* params, Slice& s) {
    s.params.clear();
    for (const auto& [k, v] : params->obj) {
        if (k == "case_c" || k == "case_k" || k == "threads" || k == "gpu") {
            continue;
        }
        switch (v.type) {
        case Json::Type::Bool:
            s.params[k] = v.b ? "true" : "false";
            break;
        case Json::Type::Null:
            s.params[k] = "null";
            break;
        case Json::Type::String:
            s.params[k] = "\"" + v.text + "\"";
            break;
        case Json::Type::Number:
            s.params[k] = v.text;
            break;
        default:
            s.params[k] = "<compound>";
        }
    }
}

/// The params block: the run's options (Slice::opts), the slice c of k and
/// the un-walked depth, each checked against the range verify writes.
bool read_params(const Json* params, Slice& s, std::string& err) {
    verify::Options& o = s.opts;
    o = verify::Options{};
    std::uint64_t depth = 0;
    if (!get_u32(params, "N", o.N, err) || !get_u32(params, "A", o.A, err) ||
        !get_u32(params, "B", o.B, err) || !get_u32(params, "bitvectors", o.num_bitvectors, err) ||
        !get_u32(params, "mod3t", o.step1.mod3t, err) ||
        !get_u32(params, "window_chains", o.window_chains, err) ||
        !get_u32(params, "window_templates_D", o.window_templates_D, err) ||
        !get_u64(params, "unwalked_depth", depth, err) || !get_u64(params, "case_c", s.c, err) ||
        !get_u64(params, "case_k", s.k, err) ||
        !get_bool(params, "partial_oee", o.partial_oee, err)) {
        return false;
    }
    // The slice count sizes the coverage scan of a merge, so it is bounded here.
    if (s.k == 0 || s.k > kMaxSlices || (s.k & (s.k - 1)) != 0) {
        err = "params.case_k is " + std::to_string(s.k) + ", not a power of two in [1, " +
              std::to_string(kMaxSlices) + "] (the slice counts verify --checkpoint writes)";
        return false;
    }
    if (s.c >= s.k) {
        err =
            "params.case_c " + std::to_string(s.c) + " is not below case_k " + std::to_string(s.k);
        return false;
    }
    if (o.N > 96 || o.A > o.N) {
        err = "params N/A out of range";
        return false;
    }
    // Un-walked depth K behind max_exact. merge() recomputes exactness from it.
    if (depth > 4096) {
        err = "params.unwalked_depth out of range";
        return false;
    }
    // Block mode: params.block_h, a decimal string; 0 is a full run.
    const Json* bh = params->get("block_h");
    if (bh == nullptr || bh->type != Json::Type::String ||
        !collatz::from_string(bh->text, o.block_h)) {
        err = "params.block_h is missing or not a decimal string";
        return false;
    }
    // table16 is the only kernel, and merged documents name it.
    const Json* kern = params->get("kernel");
    if (kern == nullptr || kern->type != Json::Type::String || kern->text != "table16") {
        err = "params.kernel is not \"table16\", the only kernel this build has";
        return false;
    }
    const Json* thr = params->get("record_threshold");
    if (thr == nullptr || (thr->type != Json::Type::Null && thr->type != Json::Type::String)) {
        err = "params.record_threshold is neither null nor a string";
        return false;
    }
    o.record_threshold = thr->type == Json::Type::String ? thr->text : std::string();
    // result_json always writes window_landing false, so a slice of a run that
    // had it on cannot be merged into a correct file.
    if (const Json* wl = params->get("window_landing");
        wl != nullptr && wl->type == Json::Type::Bool && wl->b) {
        err = "params.window_landing is true; this build has no window-landing mode";
        return false;
    }
    const Json* src = params->get("assumption_source");
    if (src == nullptr || src->type != Json::Type::String) {
        err = "params.assumption_source is missing or not a string";
        return false;
    }
    o.assumption_source = src->text;
    // Per-slice fields outside the digest; `collatz merge` writes them back, so
    // they are required rather than defaulted.
    if (!get_u32(params, "threads", o.threads, err) || !get_bool(params, "gpu", s.gpu, err)) {
        return false;
    }
    o.step1.case_c = s.c;
    o.step1.case_k = s.k;
    s.N = o.N;
    s.block_h = o.block_h;
    s.unwalked_depth = static_cast<unsigned>(depth);
    read_params_map(params, s);
    return true;
}

} // namespace

bool parse_slice(std::string_view text, Slice& s, std::string& err) {
    std::string perr;
    const std::optional<Json> doc = parse_json(text, &perr);
    if (!doc) {
        err = "not valid JSON (" + perr + ")";
        return false;
    }
    if (doc->type != Json::Type::Object) {
        err = "top-level JSON value is not an object";
        return false;
    }
    const Json* params = doc->get("params");
    const Json* res = doc->get("result");
    const Json* pre = doc->get("preflight");
    if (params == nullptr || res == nullptr || pre == nullptr) {
        err = "missing params/result/preflight block (not a `verify --json` file?)";
        return false;
    }
    s.r = verify::Result{};
    if (!read_params(params, s, err) || !read_result(res, pre, s.r, err)) {
        return false;
    }
    for (const char* key : {"backend", "commit"}) {
        if (const Json* v = doc->get(key); v == nullptr || v->type != Json::Type::String) {
            err = std::string("missing string field '") + key + "'";
            return false;
        }
    }
    s.backend = doc->get("backend")->text;
    s.commit = doc->get("commit")->text;
    s.fingerprint.clear();
    if (const Json* cp = doc->get("checkpoint"); cp != nullptr) {
        if (const Json* fpj = cp->get("fingerprint");
            fpj != nullptr && fpj->type == Json::Type::Object) {
            for (const auto& [k, v] : fpj->obj) {
                if (v.type != Json::Type::String) {
                    err = "checkpoint fingerprint value is not a string";
                    return false;
                }
                s.fingerprint[k] = v.text;
            }
        }
    }
    // Last, on the values read: a counter or parameter changed after the file
    // was written no longer matches the recorded digest.
    return check_slice_digest(res, s, err);
}

bool read_slice_file(const std::string& path, Slice& out, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) {
        return false;
    }
    out.source = path;
    return parse_slice(text, out, err);
}

} // namespace collatz::checkpoint
