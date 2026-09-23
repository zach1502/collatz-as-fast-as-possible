// digest_text_from_json: the canonical result-digest text rebuilt from the
// fields of a `verify --json` document, and its field readers.

#include "collatz/u128.hpp"
#include "collatz/worker.hpp"
#include "worker_internal.hpp"

namespace collatz::worker {

using checkpoint::Json;
using namespace detail;

namespace detail {

bool is_decimal(std::string_view s) {
    std::string canon;
    return collatz::canonical_decimal(s, canon) && canon == s && s.front() != '+';
}

} // namespace detail

namespace {

// Field readers for digest_text_from_json; each sets `err` on failure. They
// accept exactly what checkpoint::result_json writes: canonical decimal
// integers (u64 counters as JSON numbers, u128 and wider values as strings).
const Json* field(const Json* obj, const char* key, const char* path, std::string& err) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    if (v == nullptr) {
        err = std::string("missing ") + path + key;
    }
    return v;
}

bool u64_text(
    const Json* obj, const char* key, const char* path, std::string& out, std::string& err) {
    const Json* v = field(obj, key, path, err);
    u128 x = 0;
    if (v == nullptr || v->type != Json::Type::Number || !is_decimal(v->text) ||
        !collatz::from_string(v->text, x) || x > ~std::uint64_t{0}) {
        if (v != nullptr) {
            err = std::string(path) + key + " is not an integer in [0, 2^64)";
        }
        return false;
    }
    out = v->text;
    return true;
}

bool dec_string(
    const Json* obj, const char* key, const char* path, std::string& out, std::string& err) {
    const Json* v = field(obj, key, path, err);
    if (v == nullptr || v->type != Json::Type::String || !is_decimal(v->text)) {
        if (v != nullptr) {
            err = std::string(path) + key + " is not a canonical decimal string";
        }
        return false;
    }
    out = v->text;
    return true;
}

bool bool_flag(
    const Json* obj, const char* key, const char* path, std::string& out, std::string& err) {
    const Json* v = field(obj, key, path, err);
    if (v == nullptr || v->type != Json::Type::Bool) {
        if (v != nullptr) {
            err = std::string(path) + key + " is not a boolean";
        }
        return false;
    }
    out = v->b ? "1" : "0";
    return true;
}

} // namespace

bool digest_text_from_json(const Json& doc, std::string& text, std::string& err) {
    const Json* p = field(&doc, "params", "", err);
    const Json* r = field(&doc, "result", "", err);
    const Json* pre = field(&doc, "preflight", "", err);
    if (p == nullptr || r == nullptr || pre == nullptr) {
        return false;
    }
    std::string block_h, K, below, cc, ck, A, B, nbv, kernel, mod3t, poee, wc, wd, depth, thr;
    if (!dec_string(p, "block_h", "params.", block_h, err) ||
        !u64_text(p, "N", "params.", K, err) ||
        !dec_string(p, "assumes_below", "params.", below, err) ||
        !u64_text(p, "case_c", "params.", cc, err) || !u64_text(p, "case_k", "params.", ck, err) ||
        !u64_text(p, "A", "params.", A, err) || !u64_text(p, "B", "params.", B, err) ||
        !u64_text(p, "bitvectors", "params.", nbv, err) ||
        !u64_text(p, "mod3t", "params.", mod3t, err) ||
        !bool_flag(p, "partial_oee", "params.", poee, err) ||
        !u64_text(p, "window_chains", "params.", wc, err) ||
        !u64_text(p, "window_templates_D", "params.", wd, err) ||
        !u64_text(p, "unwalked_depth", "params.", depth, err)) {
        return false;
    }
    const Json* kv = field(p, "kernel", "params.", err);
    if (kv == nullptr || kv->type != Json::Type::String) {
        err = "params.kernel is not a string";
        return false;
    }
    kernel = kv->text;
    const Json* tv = field(p, "record_threshold", "params.", err);
    if (tv == nullptr || (tv->type != Json::Type::Null &&
                          (tv->type != Json::Type::String || !is_decimal(tv->text)))) {
        err = "params.record_threshold is neither null nor a decimal string";
        return false;
    }
    thr = tv->type == Json::Type::Null ? "-" : tv->text;

    std::string cand, masked, surv, steps, blocks, cov, pow, pki, mx, mxn, exceeds, overflow, trunc;
    if (!u64_text(r, "candidates_checked", "result.", cand, err) ||
        !u64_text(r, "step2_masked_out", "result.", masked, err) ||
        !u64_text(r, "step1_survivors", "result.", surv, err) ||
        !u64_text(r, "step3_steps_total", "result.", steps, err) ||
        !u64_text(r, "blocks_total", "result.", blocks, err) ||
        !u64_text(r, "coverage_checksum", "result.", cov, err) ||
        !u64_text(r, "pow_checksum", "result.", pow, err) ||
        !u64_text(r, "pow_kernel_id", "result.", pki, err) ||
        !dec_string(r, "max_excursion", "result.", mx, err) ||
        !dec_string(r, "max_excursion_n", "result.", mxn, err) ||
        !bool_flag(r, "max_exceeds_u128", "result.", exceeds, err) ||
        !u64_text(r, "overflow_fallbacks", "result.", overflow, err) ||
        !bool_flag(r, "records_truncated", "result.", trunc, err)) {
        return false;
    }
    const Json* ev = field(r, "max_exact", "result.", err);
    if (ev == nullptr || ev->type != Json::Type::Bool) {
        err = "result.max_exact is not a boolean";
        return false;
    }
    const std::string exact = ev->b ? "1" : "0";
    std::string pf[5];
    const char* pf_keys[5] = {
        "candidates", "coverage_checksum", "pow_checksum", "blocks_total", "step3_steps_total"};
    for (int i = 0; i < 5; ++i) {
        if (!u64_text(pre, pf_keys[i], "preflight.", pf[i], err)) {
            return false;
        }
    }
    const Json* rv = field(r, "records", "result.", err);
    if (rv == nullptr || rv->type != Json::Type::Array) {
        err = "result.records is not an array";
        return false;
    }
    std::string recs;
    for (const Json& e : rv->arr) {
        if (e.type != Json::Type::Array || e.arr.size() != 2 ||
            e.arr[0].type != Json::Type::String || e.arr[1].type != Json::Type::String ||
            !is_decimal(e.arr[0].text) || !is_decimal(e.arr[1].text)) {
            err = "result.records entry is not [\"n\", \"peak\"]";
            return false;
        }
        if (!recs.empty()) {
            recs += ';';
        }
        recs += e.arr[0].text + ":" + e.arr[1].text;
    }
    const Json* sv = field(r, "result_digest_schema", "result.", err);
    if (sv == nullptr || sv->type != Json::Type::String ||
        sv->text != checkpoint::kResultDigestSchema) {
        if (sv != nullptr) {
            err = std::string("result.result_digest_schema is not ") +
                  checkpoint::kResultDigestSchema;
        }
        return false;
    }
    std::string s = std::string(checkpoint::kResultDigestSchema) + "\n";
    const auto line = [&s](const char* key, const std::string& value) {
        s += key;
        s += '=';
        s += value;
        s += '\n';
    };
    line("block_h", block_h);
    line("K", K);
    line("assumes_below", below);
    line("case", cc + "/" + ck);
    line("A", A);
    line("B", B);
    line("bitvectors", nbv);
    line("kernel", kernel);
    line("mod3t", mod3t);
    line("partial_oee", poee);
    line("window_chains", wc);
    line("window_templates_D", wd);
    line("unwalked_depth", depth);
    line("record_threshold", thr);
    line("candidates_checked", cand);
    line("step2_masked_out", masked);
    line("step1_survivors", surv);
    line("step3_steps_total", steps);
    line("blocks_total", blocks);
    line("coverage_checksum", cov);
    line("pow_checksum", pow);
    line("pow_kernel_id", pki);
    line("max_excursion", mx);
    line("max_excursion_n", mxn);
    line("max_exceeds_u128", exceeds);
    line("max_exact", exact);
    line("overflow_fallbacks", overflow);
    line("preflight", pf[0] + "," + pf[1] + "," + pf[2] + "," + pf[3] + "," + pf[4]);
    line("records", recs);
    line("records_truncated", trunc);
    text = std::move(s);
    return true;
}

} // namespace collatz::worker
