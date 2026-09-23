// The lease of a unit in `collatz worker` (collatz/worker/lease.hpp): parse_lease
// and its refusal of option injection, the backend and the note, the verify
// command line, the choice of the GPU, the certify command line and selection
// rule of an audit, and the lease id.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/sha256.hpp"
#include "collatz/worker.hpp"
#include "worker_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace wk = collatz::worker;
using collatz::test::json_of;

TEST_CASE("parse_lease accepts a server lease and refuses option injection") {
    const std::string good =
        R"({"unit_id":7,"campaign_id":1,"h":"278501785601","k":33,"l":"0","end":"0",)"
        R"("a":10,"b":24,"kernel":"table16","bitvectors":8,"mod3t":6,"partial_oee":true,)"
        R"("window_chains":6,"window_templates_d":12,)"
        R"("record_threshold":"494114680693632162318569565806744468898092",)"
        R"("assumption_source":"Barina live frontier (all n below 2075 times 2^60) and earlier units",)"
        R"("lease_expires":1791450887,"args":["ignored"]})";
    const auto lease = [](const std::string& text, wk::Lease& out) {
        const std::optional<ckp::Json> j = ckp::parse_json(text);
        REQUIRE(j.has_value());
        std::string err;
        return wk::parse_lease(*j, out, err);
    };
    wk::Lease l;
    REQUIRE(lease(good, l));
    CHECK(l.unit_id == 7);
    CHECK(l.h == "278501785601");
    CHECK(l.K == 33);
    CHECK(l.backend.empty());
    CHECK(l.note.empty());
    CHECK(l.kernel == "table16");
    CHECK(l.assumption_source.find("Barina") == 0);

    const std::vector<std::string> args = wk::verify_args(l, 4, false, "CK", "OUT.json");
    // The block size is the lease's (the server's choice), not the size asked for.
    const std::vector<std::string> head = {
        "verify", "--block", "278501785601", "--K", "33", "--A", "10", "--B", "24"};
    CHECK(std::equal(head.begin(), head.end(), args.begin()));
    const auto has = [&args](const std::string& a, const std::string& b) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == a && args[i + 1] == b) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("--record-threshold", "494114680693632162318569565806744468898092"));
    CHECK(has("--assumption-source", l.assumption_source));
    CHECK(has("--threads", "4"));
    CHECK(has("--checkpoint", "CK"));
    CHECK(has("--json", "OUT.json"));
    CHECK(std::find(args.begin(), args.end(), "--no-partial-oee") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--gpu") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--progress-file") == args.end());
    const std::vector<std::string> with_progress =
        wk::verify_args(l, 4, false, "CK", "OUT.json", "PROGRESS.txt");
    REQUIRE(with_progress.size() == args.size() + 2);
    CHECK(std::equal(args.begin(), args.end(), with_progress.begin()));
    CHECK(with_progress[args.size()] == "--progress-file");
    CHECK(with_progress.back() == "PROGRESS.txt");

    const auto refused = [&](const std::string& from, const std::string& to) {
        std::string t = good;
        const std::size_t at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        wk::Lease x;
        return !lease(t, x);
    };
    CHECK(refused(R"("h":"278501785601")", R"("h":"5 --json /tmp/x")"));
    CHECK(refused(R"("h":"278501785601")", R"("h":278501785601)"));
    CHECK(refused(R"("kernel":"table16")", R"("kernel":"--no-partial-oee")"));
    CHECK(refused(R"("k":33)", R"("k":200)"));
    CHECK(refused(R"("k":33)", R"("k":31)"));
    CHECK(refused(R"("a":10)", R"("a":40)"));
    CHECK(refused(R"("record_threshold":"494)", R"("record_threshold":"-494)"));
    CHECK(refused(R"(Barina live)", R"(Barina \"live)"));
    CHECK(refused(R"(Barina live)", R"(Barina * live)"));
    CHECK(refused(R"("partial_oee":true)", R"("partial_oee":1)"));
}

TEST_CASE("parse_lease reads the backend and the note") {
    const std::string base =
        R"({"unit_id":7,"campaign_id":1,"h":"557003571203","k":32,"a":10,"b":24,"kernel":"table16",)"
        R"("bitvectors":8,"mod3t":6,"partial_oee":true,"window_chains":6,"window_templates_d":12,)"
        R"("record_threshold":null,"assumption_source":null,"lease_expires":1791450887)";
    wk::Lease l;
    std::string err;
    REQUIRE_MESSAGE(wk::parse_lease(json_of(base + "}"), l, err), err);
    CHECK(l.backend.empty());
    REQUIRE_MESSAGE(wk::parse_lease(json_of(base + R"(,"backend":"cpu","note":null})"), l, err),
                    err);
    CHECK(l.backend == "cpu");
    CHECK(l.note.empty());
    REQUIRE_MESSAGE(
        wk::parse_lease(
            json_of(base + R"(,"backend":"gpu","note":"a smaller block of 2^46"})"), l, err),
        err);
    CHECK(l.backend == "gpu");
    CHECK(l.note == "a smaller block of 2^46");
    CHECK_FALSE(wk::parse_lease(json_of(base + R"(,"backend":"--gpu"})"), l, err));
    CHECK_FALSE(wk::parse_lease(json_of(base + R"(,"backend":1})"), l, err));
    CHECK_FALSE(wk::parse_lease(json_of(base + R"(,"note":7})"), l, err));
    CHECK_FALSE(
        wk::parse_lease(json_of(base + R"(,"note":")" + std::string(401, 'x') + R"("})"), l, err));
    // A lease with idx and audit fields parses; both fields are ignored.
    REQUIRE_MESSAGE(wk::parse_lease(json_of(base + R"(,"idx":3,"audit":true})"), l, err), err);
}

TEST_CASE("lease_uses_gpu: a lease moves a GPU worker to the CPU, never a CPU worker to the GPU") {
    wk::Lease l;
    CHECK(wk::lease_uses_gpu(l, true));
    CHECK_FALSE(wk::lease_uses_gpu(l, false));
    l.backend = "gpu";
    CHECK(wk::lease_uses_gpu(l, true));
    CHECK_FALSE(wk::lease_uses_gpu(l, false));
    l.backend = "cpu";
    CHECK_FALSE(wk::lease_uses_gpu(l, true));
    CHECK_FALSE(wk::lease_uses_gpu(l, false));
    const auto has_gpu = [](const std::vector<std::string>& a) {
        return std::find(a.begin(), a.end(), "--gpu") != a.end();
    };
    l.backend = "gpu";
    l.h = "557003571203";
    l.K = 32;
    l.kernel = "table16";
    l.record_threshold = "none";
    CHECK_FALSE(has_gpu(wk::verify_args(l, 2, wk::lease_uses_gpu(l, false), "CK", "OUT.json")));
}

// The worker's audit command line and selection rule.

namespace {

constexpr const char* kDigest = "f699b5fd2dd9d98a816aa58da0d5fd10ddc19070784cb255e541fccda270618f";

} // namespace

TEST_CASE("worker: audit_args runs certify on the lease's parameters") {
    wk::Lease l;
    l.unit_id = 17;
    l.h = "557003571200";
    l.K = 32;
    l.A = 10;
    l.B = 24;
    l.bitvectors = 8;
    l.mod3t = 6;
    l.window_chains = 6;
    l.window_templates_d = 12;
    l.kernel = "table16";
    l.record_threshold = "none";
    const auto joined = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& x : v) {
            s += (s.empty() ? "" : " ") + x;
        }
        return s;
    };
    CHECK(joined(wk::audit_args(l, kDigest, 1, "r.json", "a.json")) ==
          "certify --block 557003571200 --K 32 --A 10 --B 24 --bitvectors 8 --mod3t 6 "
          "--window-chains 6 --window-templates-D 12 --threads 1 --audit-seed " +
              std::string(kDigest) + " --result r.json --json a.json --audit-cache-only");
    l.partial_oee = false;
    CHECK(joined(wk::audit_args(l, kDigest, 2, "r.json", "a.json")) ==
          "certify --block 557003571200 --K 32 --A 10 --B 24 --bitvectors 8 --mod3t 6 "
          "--window-chains 6 --window-templates-D 12 --no-partial-oee --threads 2 --audit-seed " +
              std::string(kDigest) + " --result r.json --json a.json --audit-cache-only");
}

TEST_CASE("worker: audit_selected is a function of the digest and the rate") {
    // SHA-256("collatz-audit-select|" + kDigest) starts at 0.9049... * 2^64.
    CHECK_FALSE(wk::audit_selected(kDigest, 0.0));
    CHECK_FALSE(wk::audit_selected(kDigest, 0.05));
    CHECK_FALSE(wk::audit_selected(kDigest, 0.9));
    CHECK(wk::audit_selected(kDigest, 0.91));
    CHECK(wk::audit_selected(kDigest, 1.0));
    CHECK_FALSE(wk::audit_selected(kDigest, std::numeric_limits<double>::quiet_NaN()));
    std::uint64_t picked = 0;
    const std::uint64_t n = 20000;
    for (std::uint64_t i = 0; i < n; ++i) {
        picked += wk::audit_selected(collatz::sha256_hex(std::to_string(i)), 0.05) ? 1u : 0u;
    }
    // 5% of 20000 is 1000, with a standard deviation of about 31.
    CHECK(picked > 850);
    CHECK(picked < 1150);
}

// The lease id: parsed when the server sends one, sent back in submit and
// release bodies, and the tie between a unit directory's files and a lease.

TEST_CASE("worker: the lease id is read, sent back, and keeps a unit's files to its lease") {
    namespace fs = std::filesystem;
    const std::string base =
        R"({"unit_id":7,"campaign_id":1,"h":"557003571203","k":32,"a":10,"b":24,"kernel":"table16",)"
        R"("bitvectors":8,"mod3t":6,"partial_oee":true,"window_chains":6,"window_templates_d":12,)"
        R"("record_threshold":null,"assumption_source":null,"lease_expires":1791450887)";
    wk::Lease l;
    std::string err;
    REQUIRE_MESSAGE(wk::parse_lease(json_of(base + "}"), l, err), err);
    CHECK(l.lease_id == 0);
    CHECK(wk::lease_fields(l) == R"("unit_id":7)");
    REQUIRE_MESSAGE(wk::parse_lease(json_of(base + R"(,"lease_id":4242})"), l, err), err);
    CHECK(l.lease_id == 4242);
    CHECK(wk::lease_fields(l) == R"("unit_id":7,"lease_id":4242)");
    CHECK_FALSE(wk::parse_lease(json_of(base + R"(,"lease_id":"4242"})"), l, err));
    CHECK_FALSE(wk::parse_lease(json_of(base + R"(,"lease_id":-1})"), l, err));
    CHECK_FALSE(wk::parse_lease(json_of(base + R"(,"lease_id":9007199254740992})"), l, err));

    const fs::path udir = fs::temp_directory_path() / ("collatz-worker-unit-" + wk::random_hex(6));
    const auto put = [&udir] {
        fs::create_directories(udir / "ck");
        std::string e;
        REQUIRE(ckp::write_file_atomic((udir / "ck" / "slice-0.json").string(), "{}\n", e));
        REQUIRE(ckp::write_file_atomic((udir / "result.json").string(), "{}\n", e));
    };
    const auto files = [&udir] {
        return fs::exists(udir / "ck" / "slice-0.json") && fs::exists(udir / "result.json");
    };
    put();
    wk::Lease a;
    a.lease_id = 11;
    wk::keep_unit_files(udir, a); // no lease-id file: the files are of no known lease
    CHECK_FALSE(files());
    put();
    wk::keep_unit_files(udir, a); // the same lease, also when renewed: kept
    CHECK(files());
    wk::Lease b;
    b.lease_id = 12;
    wk::keep_unit_files(udir, b); // another lease of the unit (it was started over): removed
    CHECK_FALSE(files());
    put();
    wk::keep_unit_files(udir, b);
    CHECK(files());
    std::error_code ec;
    fs::remove_all(udir, ec);
}
