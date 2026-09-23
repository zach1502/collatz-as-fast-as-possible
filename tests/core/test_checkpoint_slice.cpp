// Reading a slice file (src/core/checkpoint_slice.cpp): the sizes and
// parameters a file carries are bounded before anything uses them (a case_k
// above 1024, a value above 2^32-1), the recorded result_digest must match the
// fields read, and merge() and coverage() refuse a slice count above 1024
// before they allocate by it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace v = collatz::verify;

namespace {

// The options of a sliced `verify --N 30` run with the CLI's sieve flags.
v::Options slice_opts() {
    v::Options o;
    o.N = 30;
    o.A = 10;
    o.B = 24;
    o.threads = 4;
    o.step1.mod3t = 6;
    o.partial_oee = true;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.record_threshold = "1000";
    o.step1.case_c = 1;
    o.step1.case_k = 2;
    return o;
}

// A Result whose fields are distinct, so each one can be found in the text.
v::Result slice_result() {
    v::Result r;
    r.candidates_checked = 7777777;
    r.step2_masked_out = 5555;
    r.step3_steps_total = 4444444;
    r.coverage_checksum = 1111111111111;
    r.pow_checksum = 2222222222222;
    r.pow_available = true;
    r.pow_kernel_id = 16;
    r.blocks_total = 3333333;
    r.max_excursion = 99999999999;
    r.max_excursion_n = 888888;
    r.max_exact = true;
    r.records = {{888887, "99999999998"}};
    r.step1.survivors = 6666;
    r.preflight_candidates = 131071;
    r.preflight_coverage = 101;
    r.preflight_pow = 202;
    r.preflight_blocks = 303;
    r.preflight_step3_steps = 404;
    return r;
}

std::string slice_doc() {
    return ckp::result_json(ckp::JsonMeta{}, slice_opts(), slice_result());
}

// `doc` with its only occurrence of `from` replaced by `to`.
std::string edited(std::string doc, const std::string& from, const std::string& to) {
    const std::size_t at = doc.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    REQUIRE_MESSAGE(doc.find(from, at + 1) == std::string::npos, from);
    doc.replace(at, from.size(), to);
    return doc;
}

// parse_slice refuses `doc`, with `want` in the reason.
void refused(const std::string& doc, const std::string& want) {
    ckp::Slice s;
    std::string err;
    CHECK_FALSE(ckp::parse_slice(doc, s, err));
    CHECK_MESSAGE(err.find(want) != std::string::npos, err);
}

ckp::Slice mk(std::uint64_t c, std::uint64_t k) {
    ckp::Slice s;
    s.c = c;
    s.k = k;
    s.N = 40;
    s.unwalked_depth = 58;
    s.backend = "cpu";
    s.r.pow_available = true;
    s.r.pow_kernel_id = 16;
    s.source = "s" + std::to_string(c);
    return s;
}

} // namespace

TEST_CASE("parse_slice reads back the file verify writes") {
    ckp::Slice s;
    std::string err;
    REQUIRE_MESSAGE(ckp::parse_slice(slice_doc(), s, err), err);
    CHECK(s.c == 1);
    CHECK(s.k == 2);
    CHECK(s.r.candidates_checked == 7777777);
    CHECK(s.r.records.size() == 1);
}

TEST_CASE("parse_slice refuses a case_k that is not a slice count") {
    const std::string doc = slice_doc();
    // The fuzzed values: merge sized its coverage scan by them (std::length_error
    // at 2^62, an allocation of 4 TiB or 8 GiB at 2^40 and 2^31).
    for (const std::string k :
         {"4611686018427387904", "1099511627776", "2147483648", "2048", "0", "3"}) {
        CAPTURE(k);
        refused(edited(doc, "\"case_k\": 2\n", "\"case_k\": " + k + "\n"), "case_k");
    }
    refused(edited(doc, "\"case_c\": 1,", "\"case_c\": 2,"), "case_c");
}

TEST_CASE("parse_slice refuses parameters above 2^32-1") {
    const std::string doc = slice_doc();
    // 4294967320 = 2^32 + 24 once became B = 24 in `collatz merge`.
    refused(edited(doc, "\"B\": 24,", "\"B\": 4294967320,"), "B is 4294967320");
    refused(edited(doc, "\"A\": 10,", "\"A\": 4294967306,"), "A is 4294967306");
    refused(edited(doc, "\"bitvectors\": 8,", "\"bitvectors\": 4294967304,"), "bitvectors");
    refused(edited(doc, "\"mod3t\": 6,", "\"mod3t\": 4294967302,"), "mod3t");
    refused(edited(doc, "\"threads\": 4,", "\"threads\": 4294967300,"), "threads");
    refused(edited(doc, "\"pow_kernel_id\": 16,", "\"pow_kernel_id\": 4294967312,"),
            "pow_kernel_id");
}

TEST_CASE("parse_slice refuses a file changed after it was written") {
    const std::string doc = slice_doc();
    const char* const kDigest = "result_digest";
    // A changed counter, maximum, record, flag or parameter: each is digested.
    refused(edited(doc, "\"candidates_checked\": 7777777,", "\"candidates_checked\": 7777778,"),
            kDigest);
    refused(edited(doc, "\"coverage_checksum\": 1111111111111,", "\"coverage_checksum\": 1,"),
            kDigest);
    refused(edited(doc, "\"pow_checksum\": 2222222222222,", "\"pow_checksum\": 2,"), kDigest);
    refused(edited(doc, "\"blocks_total\": 3333333,", "\"blocks_total\": 3333334,"), kDigest);
    refused(edited(doc, "\"step3_steps_total\": 4444444,", "\"step3_steps_total\": 4,"), kDigest);
    refused(edited(doc, "\"step1_survivors\": 6666,", "\"step1_survivors\": 6667,"), kDigest);
    refused(edited(doc, "\"max_excursion\": \"99999999999\"", "\"max_excursion\": \"99999999990\""),
            kDigest);
    refused(edited(doc, "\"max_excursion_n\": \"888888\"", "\"max_excursion_n\": \"888889\""),
            kDigest);
    refused(edited(doc, "[\"888887\", \"99999999998\"]", "[\"888887\", \"99999999997\"]"), kDigest);
    refused(edited(doc, "\"max_exact\": true", "\"max_exact\": false"), kDigest);
    refused(edited(doc, "\"candidates\": 131071,", "\"candidates\": 131072,"), kDigest);
    refused(edited(doc, "\"A\": 10,", "\"A\": 11,"), kDigest);
    refused(edited(doc, "\"record_threshold\": \"1000\"", "\"record_threshold\": \"1001\""),
            kDigest);
    refused(edited(doc, "\"case_c\": 1,", "\"case_c\": 0,"), kDigest);
    // A missing, malformed or other-schema digest.
    const std::string d =
        ckp::result_digest(slice_opts(), v::unwalked_depth(slice_opts()), slice_result());
    refused(edited(doc, "\"result_digest\": \"" + d + "\"", "\"result_digest\": \"x\""), kDigest);
    refused(edited(doc, ",\n    \"result_digest\": \"" + d + "\"", ""), kDigest);
    refused(edited(doc, "\"collatz-result-digest/2\"", "\"collatz-result-digest/1\""), kDigest);
}

TEST_CASE("merge and coverage refuse a slice count above 1024 before allocating") {
    std::string err;
    v::Result m;
    // 2^62 slices made coverage() throw std::length_error; 2048 is out of range
    // too (verify --checkpoint writes at most 1024 slices).
    for (const std::uint64_t K : {std::uint64_t{2048}, std::uint64_t{1} << 62}) {
        CAPTURE(K);
        const std::vector<ckp::Slice> one{mk(0, K)};
        CHECK_FALSE(ckp::merge(one, m, err));
        CHECK_MESSAGE(err.find("not a power of two in [1, 1024]") != std::string::npos, err);
        std::vector<std::uint64_t> missing;
        std::vector<std::uint64_t> dup;
        CHECK_THROWS_AS(ckp::coverage(one, K, missing, dup), std::invalid_argument);
        CHECK(missing.empty());
    }
    std::vector<std::uint64_t> missing;
    std::vector<std::uint64_t> dup;
    CHECK_THROWS_AS(ckp::coverage({}, 0, missing, dup), std::invalid_argument);
    ckp::coverage({mk(0, 1024)}, 1024, missing, dup);
    CHECK(missing.size() == 1023);
}

TEST_CASE("parse_slice refuses a file without a member that merge writes back") {
    // None of these is in the digest; without them `collatz merge` wrote a
    // default (1 thread, no GPU, the default source, its own commit).
    const std::string doc = slice_doc();
    refused(edited(doc, "\"threads\": 4,", ""), "threads");
    refused(edited(doc, " \"gpu\": false,", ""), "gpu");
    refused(edited(doc, " \"assumption_source\": \"none (full run)\",", ""), "assumption_source");
    refused(edited(doc, "  \"commit\": \"unknown\",\n", ""), "commit");
    refused(edited(doc, "  \"backend\": \"cpu\",\n", ""), "backend");
}

TEST_CASE("merge refuses slices written by different commits") {
    std::string err;
    std::vector<ckp::Slice> two;
    for (const char* commit : {"aaaa", "bbbb"}) {
        v::Options o = slice_opts();
        o.step1.case_c = two.size();
        ckp::JsonMeta meta;
        meta.commit = commit;
        ckp::Slice s;
        REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(meta, o, slice_result()), s, err), err);
        two.push_back(s);
    }
    v::Result m;
    CHECK_FALSE(ckp::merge(two, m, err));
    CHECK_MESSAGE(err.find("commit") != std::string::npos, err);
    two[1] = two[0];
    two[1].c = 1;
    CHECK_MESSAGE(ckp::merge(two, m, err), err);
}

TEST_CASE("merge refuses block options verify would refuse, such as block_h = 2^128-1") {
    // Consistent files with digests of their own: (block_h + 1) wrapped to 0 in
    // the max_exact bound, so the merge claimed an exact maximum.
    for (const collatz::u128 h : {collatz::u128_max, collatz::u128{1}}) {
        CAPTURE(collatz::to_string(h));
        std::vector<ckp::Slice> two;
        std::string err;
        for (std::uint64_t c = 0; c < 2; ++c) {
            v::Options o = slice_opts();
            o.block_h = h; // h = 1: a block start of 2^30, valid
            o.N = 30;
            o.step1.case_c = c;
            ckp::Slice s;
            REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json({}, o, slice_result()), s, err), err);
            two.push_back(s);
        }
        v::Result m;
        if (h == 1) {
            CHECK_MESSAGE(ckp::merge(two, m, err), err);
        } else {
            CHECK_FALSE(ckp::merge(two, m, err));
            CHECK_MESSAGE(err.find("2^96") != std::string::npos, err);
        }
    }
}
