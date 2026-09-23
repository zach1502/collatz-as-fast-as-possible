// `collatz worker` JSON helpers (collatz/worker/json.hpp): the JSON -> digest-text
// rebuild the server also performs, the JSON round trip, and terminal-safe server
// text with the worker token redacted.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/sha256.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz/worker.hpp"
#include "progress_lines.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <string>

namespace v = collatz::verify;
namespace ckp = collatz::checkpoint;
namespace wk = collatz::worker;
namespace fs = std::filesystem;
using collatz::u128;
using collatz::test::read_text;

namespace {

v::Options block_options() {
    v::Options o;
    o.N = 32;
    o.A = 10;
    o.B = 24;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.block_h = 557003571200u; // 2075 * 2^28: unit 0 at 2075*2^60
    o.record_threshold = v::kBarinaRecord97PeakT;
    return o;
}

v::Result sample_result() {
    v::Result r;
    r.candidates_checked = 18446744073709551615ull; // u64 max survives the round trip
    r.step2_masked_out = 9007199254740993ull;       // 2^53 + 1, not a double
    r.step3_steps_total = 123456789012345ull;
    r.coverage_checksum = 2305843009213693950ull;
    r.pow_checksum = 1234567890123456789ull;
    r.pow_available = true;
    r.pow_kernel_id = 5;
    r.blocks_total = 42;
    r.max_excursion = collatz::u128_max;
    r.max_excursion_wide = "56649062372194325899121269007146717645316";
    r.max_excursion_n = (static_cast<u128>(2075) << 60) + 7u;
    r.max_exceeds_u128 = true;
    r.max_exact = true;
    r.overflow_fallbacks = 1;
    r.step1.survivors = 56476;
    r.records = {{r.max_excursion_n, r.max_excursion_wide}, {r.max_excursion_n + 2u, "999"}};
    return r;
}

std::string digest_of(const std::string& doc_text) {
    const std::optional<ckp::Json> doc = ckp::parse_json(doc_text);
    REQUIRE(doc.has_value());
    std::string text;
    std::string err;
    REQUIRE_MESSAGE(wk::digest_text_from_json(*doc, text, err), err);
    return text;
}

} // namespace

TEST_CASE("digest text rebuilt from the JSON equals result_digest_text") {
    const v::Options o = block_options();
    v::Result r = sample_result();
    ckp::JsonMeta meta;
    const unsigned depth = v::unwalked_depth(o);
    CHECK(digest_of(ckp::result_json(meta, o, r)) == ckp::result_digest_text(o, depth, r));

    // The edge cases of the canonical text: no records, max_exact = no, no
    // threshold, partial OEE off, another slice, a full run.
    v::Options o2 = o;
    o2.record_threshold.clear();
    o2.partial_oee = false;
    o2.step1.case_c = 3;
    o2.step1.case_k = 8;
    r.records.clear();
    r.max_exact = false;
    r.max_exceeds_u128 = false;
    r.max_excursion = static_cast<u128>(1) << 100;
    r.max_excursion_wide.clear();
    CHECK(digest_of(ckp::result_json(meta, o2, r)) ==
          ckp::result_digest_text(o2, v::unwalked_depth(o2), r));
    v::Options full = o2;
    full.block_h = 0;
    CHECK(digest_of(ckp::result_json(meta, full, r)) ==
          ckp::result_digest_text(full, v::unwalked_depth(full), r));
}

TEST_CASE("digest text: the server fixtures written by the binary verify") {
    const fs::path dir = fs::path(COLLATZ_SOURCE_DIR) / "server" / "test" / "fixtures";
    REQUIRE(fs::is_directory(dir));
    unsigned n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".json") {
            continue;
        }
        const std::string text = read_text(e.path());
        const std::optional<ckp::Json> doc = ckp::parse_json(text);
        REQUIRE(doc.has_value());
        CHECK_MESSAGE(collatz::sha256_hex(digest_of(text)) ==
                          doc->get("result")->get("result_digest")->text,
                      e.path().filename().string());
        ++n;
    }
    CHECK(n >= 2);
}

TEST_CASE("digest text: malformed fields are refused") {
    const std::string good = ckp::result_json(ckp::JsonMeta{}, block_options(), sample_result());
    const auto refused = [](std::string doc, const std::string& from, const std::string& to) {
        const std::size_t at = doc.find(from);
        REQUIRE(at != std::string::npos);
        doc.replace(at, from.size(), to);
        const std::optional<ckp::Json> j = ckp::parse_json(doc);
        REQUIRE(j.has_value());
        std::string text;
        std::string err;
        return !wk::digest_text_from_json(*j, text, err) && !err.empty();
    };
    CHECK(refused(good, "\"blocks_total\": 42", "\"blocks_total\": 042"));
    CHECK(refused(good, "\"blocks_total\": 42", "\"blocks_total\": -42"));
    CHECK(refused(good, "\"blocks_total\": 42", "\"blocks_total\": 18446744073709551616"));
    CHECK(refused(good, "\"blocks_total\": 42", "\"blocks_total\": \"42\""));
    CHECK(refused(good, "\"max_excursion\": \"5664", "\"max_excursion\": \"x5664"));
    CHECK(refused(good, "\"window_chains\": 6", "\"window_chains\": null"));
    CHECK(refused(good, "\"records_truncated\": false", "\"records_truncated\": 0"));
    CHECK(refused(good,
                  "\"result_digest_schema\": \"collatz-result-digest/2\"",
                  "\"result_digest_schema\": \"collatz-result-digest/1\""));
}

TEST_CASE("digest text starts with the schema the JSON names") {
    const std::string doc = ckp::result_json(ckp::JsonMeta{}, block_options(), sample_result());
    CHECK(doc.find(std::string("\"result_digest_schema\": \"") + ckp::kResultDigestSchema + "\"") !=
          std::string::npos);
    CHECK(digest_of(doc).rfind(std::string(ckp::kResultDigestSchema) + "\n", 0) == 0);
}

TEST_CASE("json_dump keeps number text and round-trips") {
    const std::string src =
        R"({"a":18446744073709551615,"b":[1.5e3,true,null,"x\"y\n\u0001"],"c":{}})";
    const std::optional<ckp::Json> j = ckp::parse_json(src);
    REQUIRE(j.has_value());
    const std::string out = wk::json_dump(*j);
    CHECK(out == R"({"a":18446744073709551615,"b":[1.5e3,true,null,"x\"y\n\u0001"],"c":{}})");
    const std::optional<ckp::Json> back = ckp::parse_json(out);
    REQUIRE(back.has_value());
    CHECK(wk::json_dump(*back) == out);
    CHECK(wk::json_quote("tab\there") == "\"tab\\there\"");
}

TEST_CASE("printable keeps server text out of terminal control sequences") {
    CHECK(wk::printable("plain text 123") == "plain text 123");
    CHECK(wk::printable("bad\x1b[2Jterm\r\nx\x7f") == "bad?[2Jterm??x?");
    CHECK(wk::printable("caf\xc3\xa9") == "caf??");
    CHECK(wk::printable(std::string(500, 'a'), 10) == "aaaaaaaaaa...");
}

TEST_CASE("redact and printable keep the worker token out of printed text") {
    // A valid token may hold '"' and '\', which a JSON string escapes.
    const std::string tok = "tok-0123456789abcdef0123456789abcdef\"\\x";
    REQUIRE(wk::valid_worker_token(tok));
    wk::set_secret(tok);
    CHECK(wk::printable("bad token " + tok + " here") == "bad token [token] here");
    CHECK(wk::redact(tok + tok) == "[token][token]");
    CHECK(wk::printable(wk::json_quote("echo " + tok)) == "\"echo [token]\"");
    CHECK(wk::printable("abc " + tok, 6) == "abc [t..."); // the cut comes after the redaction
    CHECK(wk::redact("> Authorization: Bearer abc.def\r\nX: y") ==
          "> Authorization: [redacted]\r\nX: y");
    CHECK(wk::redact(R"({"authorization": "Basic c2VjcmV0", "x": 1})") ==
          R"({"authorization": "[redacted]", "x": 1})");
    CHECK(wk::redact("AUTHORIZATION=secret") == "AUTHORIZATION=[redacted]");
    CHECK(wk::redact("the Authorization header is missing") ==
          "the Authorization header is missing");
    wk::set_secret("");
    CHECK(wk::redact("x " + tok) == "x " + tok);
}
