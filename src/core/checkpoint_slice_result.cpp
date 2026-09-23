// Reading the result and preflight blocks of a slice file (read_result, for
// parse_slice) and their field readers.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

#include <cstdlib>

namespace collatz::checkpoint {

using namespace detail;

namespace {

bool get_u128_str(const Json* obj, const char* key, u128& out, std::string& err) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    if (v == nullptr || (v->type != Json::Type::String && v->type != Json::Type::Number) ||
        !collatz::from_string(v->text, out)) {
        err = std::string("missing u128 field '") + key + "'";
        return false;
    }
    return true;
}

double get_double_or0(const Json* obj, const char* key) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    if (v == nullptr || v->type != Json::Type::Number) {
        return 0.0;
    }
    return std::strtod(v->text.c_str(), nullptr);
}

/// result.max_excursion: a decimal string of any size. Above 2^128-1 it lands in
/// max_excursion_wide with max_excursion = u128_max (the Result convention).
bool get_max_excursion(const Json* obj, verify::Result& r, std::string& err) {
    const Json* v = obj != nullptr ? obj->get("max_excursion") : nullptr;
    std::string dec;
    if (v == nullptr || (v->type != Json::Type::String && v->type != Json::Type::Number) ||
        !collatz::canonical_decimal(v->text, dec)) {
        err = "missing decimal field 'max_excursion'";
        return false;
    }
    r.max_excursion_wide.clear();
    if (!collatz::from_string(dec, r.max_excursion)) {
        r.max_excursion = u128_max;
        r.max_excursion_wide = dec;
    }
    return true;
}

/// result.records: [["n", "peak"], ...]; absent means none.
bool get_records(const Json* obj, std::vector<verify::RecordHit>& out, std::string& err) {
    out.clear();
    const Json* v = obj != nullptr ? obj->get("records") : nullptr;
    if (v == nullptr) {
        return true;
    }
    if (v->type != Json::Type::Array) {
        err = "result.records is not an array";
        return false;
    }
    for (const Json& e : v->arr) {
        verify::RecordHit h;
        if (e.type != Json::Type::Array || e.arr.size() != 2 ||
            e.arr[0].type != Json::Type::String || e.arr[1].type != Json::Type::String ||
            !collatz::from_string(e.arr[0].text, h.n) ||
            !collatz::canonical_decimal(e.arr[1].text, h.peak)) {
            err = "result.records entry is not [\"n\", \"peak\"]";
            return false;
        }
        out.push_back(std::move(h));
    }
    return true;
}

} // namespace

namespace detail {

bool read_result(const Json* res, const Json* pre, verify::Result& r, std::string& err) {
    std::uint64_t pki = 0;
    std::uint64_t kk = 0;
    if (!get_u64(res, "candidates_checked", r.candidates_checked, err) ||
        !get_u64(res, "step2_masked_out", r.step2_masked_out, err) ||
        !get_u64(res, "step3_steps_total", r.step3_steps_total, err) ||
        !get_u64(res, "coverage_checksum", r.coverage_checksum, err) ||
        !get_u64(res, "pow_checksum", r.pow_checksum, err) ||
        !get_bool(res, "pow_available", r.pow_available, err) ||
        !get_u64(res, "pow_kernel_id", pki, err) ||
        !get_u64(res, "blocks_total", r.blocks_total, err) || !get_max_excursion(res, r, err) ||
        !get_u128_str(res, "max_excursion_n", r.max_excursion_n, err) ||
        !get_bool(res, "max_exceeds_u128", r.max_exceeds_u128, err) ||
        !get_u64(res, "overflow_fallbacks", r.overflow_fallbacks, err) ||
        !get_u64(res, "step1_survivors", kk, err) ||
        !get_u64(pre, "candidates", r.preflight_candidates, err) ||
        !get_u64(pre, "coverage_checksum", r.preflight_coverage, err) ||
        !get_u64(pre, "pow_checksum", r.preflight_pow, err) ||
        !get_u64(pre, "blocks_total", r.preflight_blocks, err) ||
        !get_u64(pre, "step3_steps_total", r.preflight_step3_steps, err) ||
        !get_records(res, r.records, err)) {
        return false;
    }
    if (pki > 0xFFFFFFFFu) {
        err = "result.pow_kernel_id out of range";
        return false;
    }
    r.step1.survivors = kk;
    r.pow_kernel_id = static_cast<unsigned>(pki);
    std::uint64_t pw = 0;
    std::string ignore;
    if (get_u64(res, "prefix_walks", pw, ignore)) {
        r.prefix_walks = pw;
    }
    (void) get_bool(res, "max_exact", r.max_exact, ignore);
    (void) get_bool(res, "bv_cache_hit", r.bv_cache_hit, ignore);
    // Absent means not truncated.
    r.records_truncated = false;
    (void) get_bool(res, "records_truncated", r.records_truncated, ignore);
    r.seconds_total = get_double_or0(res, "seconds_total");
    r.seconds_step1_2 = get_double_or0(res, "seconds_step1_2");
    r.seconds_filter = get_double_or0(res, "seconds_filter");
    return true;
}

} // namespace detail

} // namespace collatz::checkpoint
