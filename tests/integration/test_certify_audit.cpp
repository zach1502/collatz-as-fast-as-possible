#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "certify_audit.hpp"
#include "collatz/certify.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/sha256.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz/worker.hpp"
#include "test_env.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Random audit of a block run (certify::audit) on the K = 32 block at
// 2075*2^60 (H = 2075*2^28, the block of the CPU/GPU digest pin): a clean audit
// that does not depend on the thread count, a sample that is a function of the
// seed, and planted errors (a dropped candidate, unlifted Step-2 masks, a wrong
// maximum, a missing record) that the audit reports when its sample reaches
// them. The last case runs `collatz certify --audit-seed S --result FILE`
// through the binary of this build on edited copies of the block's `verify
// --json` file.

using namespace collatz::test::certify_audit;

TEST_CASE("audit: a K=32 block at 2075*2^60 is clean for any thread count") {
    REQUIRE_FALSE(block_run().records.empty());
    const cf::AuditOptions a = reported("test-seed-1");
    const cf::AuditReport r4 = cf::audit(audit_opts(4), a);
    show(r4);
    CHECK(r4.clean());
    CHECK(r4.failures.empty());
    CHECK(r4.M == kM);
    CHECK(r4.L == (kH << kK));
    CHECK(r4.samples == kSamples);
    CHECK(r4.columns <= r4.samples);
    CHECK(r4.columns * 100 > r4.samples * 99);
    CHECK(r4.audited_numbers == (r4.columns << 10));
    CHECK(r4.step1_classes > 0);
    CHECK(r4.survivors > 0);
    CHECK(r4.audited_classes == r4.step1_classes + r4.survivors);
    // Every extension of a sampled survivor is either walked or re-derived.
    CHECK(r4.exclusions_checked + r4.candidates_walked == r4.step1_classes + (r4.survivors << 10));
    CHECK(r4.candidates_walked > 0);
    CHECK(r4.peaks_checked == r4.candidates_walked);
    MESSAGE("audit: " << r4.columns << " residues, " << r4.step1_classes << " Step-1 classes, "
                      << r4.survivors << " survivors, " << r4.exclusions_checked
                      << " exclusions re-derived, " << r4.candidates_walked << " walks, "
                      << r4.records_checked << " record look-ups, " << r4.seconds << " s");

    const cf::AuditReport r2 = cf::audit(audit_opts(2), a);
    CHECK(r2.clean());
    CHECK(r2.columns == r4.columns);
    CHECK(r2.step1_classes == r4.step1_classes);
    CHECK(r2.survivors == r4.survivors);
    CHECK(r2.exclusions_checked == r4.exclusions_checked);
    CHECK(r2.candidates_walked == r4.candidates_walked);
    CHECK(r2.records_checked == r4.records_checked);
}

TEST_CASE("audit: the sample is a function of the seed") {
    const std::set<u128> a = drawn("test-seed-1", kSamples);
    CHECK(a == drawn("test-seed-1", kSamples));
    const std::set<u128> b = drawn("test-seed-2", kSamples);
    std::size_t both = 0;
    for (const u128 r : a) {
        both += b.count(r);
    }
    // Two independent samples of 2^20 residues share about samples^2 / 2^20.
    CHECK(both * 8 < a.size());
    for (const u128 r : a) {
        CHECK((r & 3u) == 3u);
        CHECK(r < (static_cast<u128>(1) << kM));
    }
}

TEST_CASE("audit: a dropped candidate is caught when its residue is drawn") {
    const auto [seed, hits] = seed_drawing_records();
    REQUIRE_FALSE(hits.empty());
    // The drawn record holder with the largest excursion: a candidate the run
    // walked, which no certificate in the window covers.
    const v::RecordHit* h = &hits[0];
    for (const v::RecordHit& x : hits) {
        if (x.peak.size() > h->peak.size() ||
            (x.peak.size() == h->peak.size() && x.peak > h->peak)) {
            h = &x;
        }
    }
    cf::Options o = audit_opts(4);
    o.test_drop_n = h->n;
    const cf::AuditReport r = cf::audit(o, reported(seed));
    show(r);
    CHECK(r.failures_total == 1);
    REQUIRE(r.failures.size() == 1);
    CHECK(r.failures[0].n == collatz::to_string(h->n));
    // The same audit without the hook is clean.
    CHECK(cf::audit(audit_opts(4), reported(seed)).clean());
}

TEST_CASE("audit: unlifted Step-2 masks are caught") {
    // Every failing window replay walks its whole chain search, so this audit
    // draws fewer residues.
    cf::Options o = audit_opts(4);
    o.test_unlifted_mask = true;
    cf::AuditOptions a = reported("test-seed-1");
    a.samples = 1024;
    const cf::AuditReport r = cf::audit(o, a);
    CHECK(r.failures_total > 0);
    // A report lists at most 64 failures.
    CHECK(r.failures.size() == std::min<std::uint64_t>(r.failures_total, 64));
    MESSAGE("unlifted masks: " << r.failures_total << " failures in " << r.survivors
                               << " sampled survivors");
}

TEST_CASE("audit: a wrong maximum and a missing record are caught") {
    const auto [seed, hits] = seed_drawing_records();
    REQUIRE_FALSE(hits.empty());

    // A reported maximum at the threshold: a walked record holder exceeds it.
    cf::AuditOptions a = reported(seed);
    a.max_excursion = kThreshold;
    const cf::AuditReport low = cf::audit(audit_opts(4), a);
    CHECK(low.failures_total > 0);
    for (const cf::Failure& f : low.failures) {
        CHECK(f.reason.rfind("walk: peak ", 0) == 0);
    }

    // The record list without its entries: each walked holder is missing.
    a = reported(seed);
    a.records.clear();
    const cf::AuditReport missing = cf::audit(audit_opts(4), a);
    CHECK(missing.failures_total > 0);
    CHECK(missing.failures_total == missing.records_checked);
    for (const cf::Failure& f : missing.failures) {
        CHECK(f.reason.rfind("records: ", 0) == 0);
    }
    // A truncated list only vouches for n up to its last entry.
    a.records_truncated = true;
    CHECK(cf::audit(audit_opts(4), a).clean());
}

namespace {

namespace ckp = collatz::checkpoint;
namespace wk = collatz::worker;
namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// `text` with its first `from` replaced by `to`; with `redigest` its
/// result_digest is then recomputed from the fields as the work server does
/// it, so that only the audit's own checks can find the edit.
std::string edit(std::string text, const std::string& from, const std::string& to, bool redigest) {
    const std::size_t at = text.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    text.replace(at, from.size(), to);
    if (redigest) {
        const std::optional<ckp::Json> doc = ckp::parse_json(text);
        std::string canon;
        std::string err;
        REQUIRE((doc && wk::digest_text_from_json(*doc, canon, err)));
        const std::string old = wk::string_member(doc->get("result"), "result_digest");
        text = edit(text, old, collatz::sha256_hex(canon), false);
    }
    return text;
}

/// The decimal x + d of a decimal x below 2^128.
std::string plus(const std::string& x, int d) {
    u128 v = 0;
    REQUIRE(collatz::from_string(x, v));
    return collatz::to_string(d < 0 ? v - static_cast<u128>(-d) : v + static_cast<u128>(d));
}

} // namespace

TEST_CASE("certify --result: a changed result file is refused or fails the audit") {
    // build/<preset>/collatz, next to the tests/ directory of this test.
    fs::path bin = fs::path(wk::self_executable()).parent_path().parent_path() / "collatz";
    bin += ".exe";
    REQUIRE_MESSAGE(fs::exists(bin), "no collatz binary at " << bin.string());
    const fs::path dir = fs::temp_directory_path() / ("collatz-certify-cli-" + wk::random_hex(6));
    fs::create_directories(dir);
    if (std::getenv("COLLATZ_CACHE_DIR") == nullptr) { // run outside ctest
        collatz::test::set_env("COLLATZ_CACHE_DIR", (dir / "cache").string().c_str());
    }
    const auto cli = [&](const char* cmd, const std::vector<std::string>& more) {
        std::vector<std::string> argv = {
            bin.string(), cmd, "--block", collatz::to_string(kH), "--K", "32", "--threads", "4"};
        argv.insert(argv.end(), more.begin(), more.end());
        wk::ProcessOptions po;
        po.stdout_path = (dir / "log.txt").string();
        po.stderr_path = po.stdout_path;
        std::string err;
        return wk::run_process(argv, po, err);
    };
    const std::string run = (dir / "run.json").string();
    REQUIRE(cli("verify", {"--record-threshold", kThreshold, "--json", run}) == 0);
    const std::string text = slurp(run);
    const std::optional<ckp::Json> doc = ckp::parse_json(text);
    REQUIRE(doc.has_value());
    const ckp::Json* res = doc->get("result");
    const std::string digest = wk::string_member(res, "result_digest");
    const std::string top = wk::string_member(res, "max_excursion_n");
    const std::string max = wk::string_member(res, "max_excursion");
    const std::string cand = res->get("candidates_checked")->text;
    const std::vector<ckp::Json>& recs = res->get("records")->arr;
    REQUIRE(recs.size() > 100);
    const std::string rec_peak = recs[0].arr[1].text;

    // Audits `t` as the run's result; the audit JSON goes to dir/audit.json.
    const fs::path audit_json = dir / "audit.json";
    const auto audit = [&](const std::string& t) {
        const fs::path f = dir / "edited.json";
        std::ofstream(f, std::ios::binary) << t;
        fs::remove(audit_json);
        return cli("certify",
                   {"--audit-seed", "s", "--result", f.string(), "--json", audit_json.string()});
    };

    // The file verify wrote: clean, with the digest recomputed from its fields
    // and the argmax and every listed record walked again.
    CHECK(audit(text) == 0);
    const std::optional<ckp::Json> rep = ckp::parse_json(slurp(audit_json));
    REQUIRE(rep.has_value());
    CHECK(wk::string_member(&*rep, "result_digest") == digest);
    const ckp::Json* walked = rep->get("excursions_recomputed");
    CHECK((walked != nullptr && walked->text == std::to_string(1 + recs.size())));

    // Refused (exit 2): a digest that is not the fields', an empty maximum or
    // threshold, another kernel even with a matching digest.
    const std::string other = std::string(1, digest[0] == '0' ? '1' : '0') + digest.substr(1);
    CHECK(audit(edit(text, digest, other, false)) == 2);
    CHECK(audit(edit(text, "\"" + max + "\"", "\"\"", false)) == 2);
    CHECK(audit(edit(text, std::string("\"") + kThreshold + "\"", "\"\"", false)) == 2);
    CHECK(audit(edit(text, "\"table16\"", "\"table17\"", true)) == 2);
    CHECK_FALSE(fs::exists(audit_json));

    // Edits with a recomputed digest: the recomputed figures disagree (exit 1).
    const auto fails_with = [&](const std::string& t, const char* why) {
        CHECK(audit(t) == 1);
        CHECK_MESSAGE(slurp(audit_json).find(why) != std::string::npos, why);
    };
    fails_with(edit(text, "\"" + top + "\"", "\"" + plus(top, -4) + "\"", true),
               "max_excursion_n: its excursion");
    fails_with(edit(text, "\"" + max + "\"", "\"" + plus(max, -1) + "\"", true),
               "max_excursion_n: its excursion");
    fails_with(edit(text,
                    "\"candidates_checked\": " + cand,
                    "\"candidates_checked\": " + plus(cand, 1),
                    true),
               "counters: ");
    fails_with(edit(text, "\"" + rec_peak + "\"", "\"" + plus(rec_peak, 1) + "\"", true),
               "records: the listed peak is not the excursion");

    // Empty paths are refused; a --json path the rename cannot replace leaves
    // no temp file; --help prints the usage.
    CHECK(cli("certify", {"--audit-seed", "s", "--result", ""}) == 2);
    CHECK(cli("certify", {"--audit-seed", "s", "--result", run, "--json", ""}) == 2);
    fs::create_directories(dir / "out");
    CHECK(cli("certify",
              {"--audit-seed", "s", "--result", run, "--json", (dir / "out").string()}) == 2);
    CHECK_FALSE(fs::exists(dir / "out.tmp"));
    CHECK(cli("certify", {"--help"}) == 0);
    CHECK(slurp(dir / "log.txt").find("usage: collatz certify") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
