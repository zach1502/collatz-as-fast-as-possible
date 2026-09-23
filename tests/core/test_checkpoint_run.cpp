// Pause/resume (`verify --checkpoint DIR`): the default slice count and an
// in-process end-to-end run at small N: sliced == unsharded on every mergeable
// scalar, an interrupted run resumes without redoing finished slices, partial
// temp files and corrupt slice files are ignored/redone (also a slice changed
// after it was written), a run.json out of bounds is refused, and a pause
// request stops after the current slice.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "checkpoint_helpers.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/verify.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace fs = std::filesystem;
using collatz::test::check_same;
using collatz::test::small_opts;

namespace {

std::string tmpdir(const char* tag) {
    const fs::path p = fs::temp_directory_path() / ("collatz_ckpt_" + std::string(tag) + "_" +
                                                    std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p.string();
}

ckp::RunConfig cfg_for(const std::string& dir, const collatz::verify::Options& o, unsigned K) {
    ckp::RunConfig c;
    c.dir = dir;
    c.K = K;
    c.K_explicit = true;
    c.opts = o;
    c.meta.commit = "test";
    c.fingerprint = ckp::make_fingerprint(o, false, "test");
    c.command_line = "collatz verify (test)";
    return c;
}

} // namespace

TEST_CASE("default_slices / max_slices") {
    CHECK(ckp::default_slices(44, 12, true) == 1);
    CHECK(ckp::default_slices(56, 12, true) == 16);
    CHECK(ckp::default_slices(48, 10, false) == 4);
    CHECK(ckp::default_slices(80, 10, false) == 1024);
    CHECK(ckp::max_slices(14, 10) == 4); // M=4 -> 2 bits
    CHECK(ckp::default_slices(70, 64, false) == ckp::max_slices(70, 64));
}

TEST_CASE("checkpointed run == unsharded; interrupted run resumes; temp/corrupt files") {
    const collatz::verify::Options o = small_opts(28);
    const collatz::verify::Result full = collatz::verify::run(o);
    const std::string dir = tmpdir("e2e");
    const auto plain = [](const collatz::verify::Options& so, std::string& be) {
        be = "cpu";
        return collatz::verify::run(so);
    };
    std::string err;
    collatz::verify::Result merged;

    // Interrupt after 3 slices (runner throws on the 4th, like a crash).
    int calls = 0;
    const auto crashy = [&](const collatz::verify::Options& so, std::string& be) {
        if (++calls == 4) {
            throw std::runtime_error("simulated crash");
        }
        return plain(so, be);
    };
    CHECK(ckp::run(cfg_for(dir, o, 8), crashy, merged, err) == ckp::Outcome::Error);
    int present = 0;
    for (unsigned c = 0; c < 8; ++c) {
        present += fs::exists(fs::path(dir) / ckp::slice_file_name(c, 8)) ? 1 : 0;
    }
    CHECK(present == 3);

    // A partial temp file and a corrupt (truncated) slice file left behind.
    {
        std::ofstream(fs::path(dir) / (ckp::slice_file_name(3, 8) + ".tmp")) << "{\"sche";
        std::ofstream(fs::path(dir) / ckp::slice_file_name(2, 8)) << "{\"schema\": \"collatz";
    }

    // Resume: only slices 2 (corrupt) and 3..7 run.
    int resumed = 0;
    const auto counting = [&](const collatz::verify::Options& so, std::string& be) {
        ++resumed;
        return plain(so, be);
    };
    ckp::RunConfig rc = cfg_for(dir, o, 8);
    rc.K_explicit = false; // adopt K from run.json, as a bare re-run would
    REQUIRE(ckp::run(rc, counting, merged, err) == ckp::Outcome::Complete);
    CHECK(resumed == 6);
    CHECK_FALSE(fs::exists(fs::path(dir) / (ckp::slice_file_name(3, 8) + ".tmp")));
    check_same(merged, full);
    CHECK(fs::exists(fs::path(dir) / "merged.json"));

    // Re-running a finished checkpoint does no work and gives the same Result.
    resumed = 0;
    REQUIRE(ckp::run(cfg_for(dir, o, 8), counting, merged, err) == ckp::Outcome::Complete);
    CHECK(resumed == 0);
    check_same(merged, full);

    // Different parameters into the same DIR are refused; --checkpoint-force
    // starts over.
    collatz::verify::Options o2 = o;
    o2.step1.mod3t = 0;
    ckp::RunConfig bad = cfg_for(dir, o2, 8);
    CHECK(ckp::run(bad, plain, merged, err) == ckp::Outcome::Error);
    CHECK(err.find("mod3t") != std::string::npos);
    ckp::RunConfig wrongk = cfg_for(dir, o, 16);
    CHECK(ckp::run(wrongk, plain, merged, err) == ckp::Outcome::Error);
    bad.force = true;
    REQUIRE(ckp::run(bad, plain, merged, err) == ckp::Outcome::Complete);
    check_same(merged, collatz::verify::run(o2));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pause request stops after the current slice; slice ranges merge") {
    const collatz::verify::Options o = small_opts(26);
    const collatz::verify::Result full = collatz::verify::run(o);
    std::string err;
    collatz::verify::Result merged;
    const std::string dir = tmpdir("pause");
    int calls = 0;
    const auto pausing = [&](const collatz::verify::Options& so, std::string& be) {
        if (++calls == 2) {
            ckp::pause_flag().store(1); // Ctrl-C arrives during slice 2
        }
        be = "cpu";
        return collatz::verify::run(so);
    };
    ckp::pause_flag().store(0);
    CHECK(ckp::run(cfg_for(dir, o, 4), pausing, merged, err) == ckp::Outcome::Paused);
    CHECK(calls == 2); // slice 2 finished and was written, nothing new started
    CHECK(fs::exists(fs::path(dir) / ckp::slice_file_name(1, 4)));
    CHECK_FALSE(fs::exists(fs::path(dir) / ckp::slice_file_name(2, 4)));
    ckp::pause_flag().store(0);

    // DIR/PAUSE also pauses (checked before each slice); a restart clears it.
    // on_ready (where the CLI starts its PAUSE watcher) runs only after the
    // stale file is gone, so the old file is never announced as a new request.
    std::ofstream(fs::path(dir) / "PAUSE") << "";
    ckp::RunConfig rc2 = cfg_for(dir, o, 4);
    int ready_calls = 0;
    bool pause_seen_at_ready = true;
    rc2.on_ready = [&] {
        ++ready_calls;
        pause_seen_at_ready = fs::exists(fs::path(dir) / "PAUSE");
    };
    ckp::MergeInfo mi;
    REQUIRE(ckp::run(rc2, pausing, merged, err, &mi) == ckp::Outcome::Complete);
    CHECK(ready_calls == 1);
    CHECK_FALSE(pause_seen_at_ready);
    CHECK(mi.backend == "cpu");
    check_same(merged, full);

    // Two machines: slices 0:2 and 2:4 in separate DIRs, merged afterwards.
    const std::string d1 = tmpdir("r1");
    const std::string d2 = tmpdir("r2");
    const auto plain = [](const collatz::verify::Options& so, std::string& be) {
        be = "cpu";
        return collatz::verify::run(so);
    };
    ckp::RunConfig a = cfg_for(d1, o, 4);
    a.lo = 0;
    a.hi = 2;
    a.range_explicit = true;
    ckp::RunConfig b = cfg_for(d2, o, 4);
    b.lo = 2;
    b.hi = 4;
    b.range_explicit = true;
    REQUIRE(ckp::run(a, plain, merged, err) == ckp::Outcome::RangeDone);
    std::vector<ckp::Slice> only1;
    REQUIRE(ckp::load_slices({d1}, only1, err));
    CHECK_FALSE(ckp::merge(only1, merged, err)); // gap reported
    CHECK(err.find("missing (2): 2 3") != std::string::npos);
    REQUIRE(ckp::run(b, plain, merged, err) == ckp::Outcome::RangeDone);
    std::vector<ckp::Slice> both;
    REQUIRE(ckp::load_slices({d1, d2}, both, err));
    REQUIRE(ckp::merge(both, merged, err));
    check_same(merged, full);
    std::vector<ckp::Slice> twice;
    REQUIRE(ckp::load_slices({d1, d2, d1}, twice, err));
    CHECK_FALSE(ckp::merge(twice, merged, err)); // overlap refused

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(d1, ec);
    fs::remove_all(d2, ec);
}

TEST_CASE("a slice changed after it was written is redone on resume, not merged") {
    const collatz::verify::Options o = small_opts(24);
    const collatz::verify::Result full = collatz::verify::run(o);
    const std::string dir = tmpdir("changed");
    int calls = 0;
    const auto counting = [&](const collatz::verify::Options& so, std::string& be) {
        ++calls;
        be = "cpu";
        return collatz::verify::run(so);
    };
    std::string err;
    collatz::verify::Result merged;
    REQUIRE(ckp::run(cfg_for(dir, o, 4), counting, merged, err) == ckp::Outcome::Complete);
    // One more candidate in slice 1: valid JSON, but its result_digest no
    // longer matches its fields.
    const fs::path p = fs::path(dir) / ckp::slice_file_name(1, 4);
    std::string text;
    {
        std::ifstream in(p, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string key = "\"candidates_checked\": ";
    const std::size_t at = text.find(key);
    REQUIRE(at != std::string::npos);
    const std::size_t end = text.find(',', at);
    const std::uint64_t cand = std::stoull(text.substr(at + key.size(), end - at - key.size()));
    text.replace(at, end - at, key + std::to_string(cand + 1));
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    ckp::Slice s;
    CHECK_FALSE(ckp::read_slice_file(p.string(), s, err));
    CHECK(err.find("result_digest") != std::string::npos);
    std::vector<ckp::Slice> loaded;
    CHECK_FALSE(ckp::load_slices({dir}, loaded, err));

    calls = 0;
    REQUIRE(ckp::run(cfg_for(dir, o, 4), counting, merged, err) == ckp::Outcome::Complete);
    CHECK(calls == 1); // only the changed slice
    check_same(merged, full);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("a run.json whose slice count or range is out of bounds is refused") {
    const collatz::verify::Options o = small_opts(20);
    const auto plain = [](const collatz::verify::Options& so, std::string& be) {
        be = "cpu";
        return collatz::verify::run(so);
    };
    std::string err;
    collatz::verify::Result merged;
    for (const std::string bad :
         {"\"slices\": 4611686018427387904", "\"slices\": 3", "\"slice_hi\": 9"}) {
        CAPTURE(bad);
        const std::string dir = tmpdir("runjson");
        REQUIRE(ckp::run(cfg_for(dir, o, 4), plain, merged, err) == ckp::Outcome::Complete);
        const fs::path rp = fs::path(dir) / "run.json";
        std::string text;
        {
            std::ifstream in(rp, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string key = bad.substr(0, bad.find(':') + 2);
        const std::size_t at = text.find(key);
        REQUIRE(at != std::string::npos);
        text.replace(at, text.find(',', at) - at, bad);
        std::ofstream(rp, std::ios::binary | std::ios::trunc) << text;
        ckp::RunConfig rc = cfg_for(dir, o, 4);
        rc.K_explicit = false;
        CHECK(ckp::run(rc, plain, merged, err) == ckp::Outcome::Error);
        CHECK_MESSAGE(err.find("run.json") != std::string::npos, err);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}
