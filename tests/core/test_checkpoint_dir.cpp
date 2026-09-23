// The checkpoint directory (src/core/checkpoint_dir.cpp, checkpoint_io.cpp): a
// resume redoes a slice whose params or commit differ from the run's even when
// its fingerprint was copied from run.json; a non-empty DIR without run.json is
// refused unless forced, and only this program's temp names are removed; a
// PAUSE that cannot be removed is an error; write_file_atomic leaves no temp
// file when its rename fails; and slice and run.json paths that are not
// regular files, or are larger than any file this program writes, are refused
// before they are read.
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
#include <string>
#include <system_error>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace fs = std::filesystem;
using collatz::test::check_same;
using collatz::test::small_opts;

namespace {

fs::path tmpdir(const char* tag) {
    const fs::path p = fs::temp_directory_path() / ("collatz_ckdir_" + std::string(tag) + "_" +
                                                    std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

ckp::RunConfig cfg_for(const fs::path& dir,
                       const collatz::verify::Options& o,
                       unsigned K,
                       const std::string& commit = "test") {
    ckp::RunConfig c;
    c.dir = dir.string();
    c.K = K;
    c.K_explicit = true;
    c.opts = o;
    c.meta.commit = commit;
    c.fingerprint = ckp::make_fingerprint(o, false, commit);
    c.command_line = "collatz verify (test)";
    return c;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Counts the slices a run computes.
struct Counting {
    int calls = 0;
    ckp::SliceRunner runner() {
        return [this](const collatz::verify::Options& so, std::string& be) {
            ++calls;
            be = "cpu";
            return collatz::verify::run(so);
        };
    }
};

// Copies every slice of `from` into `to`, each with the checkpoint block (and
// so the fingerprint) of `to`'s own slice. That block is outside the result
// digest, so the copies still pass parse_slice.
void forge_slices(const fs::path& from, const fs::path& to, unsigned K) {
    const std::string key = "\"checkpoint\": {";
    for (unsigned c = 0; c < K; ++c) {
        const std::string theirs = read_text(from / ckp::slice_file_name(c, K));
        const std::string ours = read_text(to / ckp::slice_file_name(c, K));
        REQUIRE(theirs.find(key) != std::string::npos);
        REQUIRE(ours.find(key) != std::string::npos);
        std::ofstream(to / ckp::slice_file_name(c, K), std::ios::binary | std::ios::trunc)
            << theirs.substr(0, theirs.find(key)) + ours.substr(ours.find(key));
    }
    fs::remove(to / "merged.json");
}

} // namespace

TEST_CASE("resume redoes slices of other params or another commit under a copied fingerprint") {
    const collatz::verify::Options o = small_opts(22);
    collatz::verify::Options other = o;
    other.step1.mod3t = 0;
    const collatz::verify::Result full = collatz::verify::run(o);
    for (const bool by_commit : {false, true}) {
        CAPTURE(by_commit);
        const fs::path a = tmpdir("forge_a");
        const fs::path b = tmpdir("forge_b");
        Counting count;
        std::string err;
        collatz::verify::Result merged;
        REQUIRE(ckp::run(cfg_for(a, o, 2), count.runner(), merged, err) == ckp::Outcome::Complete);
        const ckp::RunConfig src =
            by_commit ? cfg_for(b, o, 2, "another-commit") : cfg_for(b, other, 2);
        REQUIRE(ckp::run(src, count.runner(), merged, err) == ckp::Outcome::Complete);
        forge_slices(b, a, 2);
        // `collatz merge` reads the copies as the run they came from.
        std::vector<ckp::Slice> copies;
        REQUIRE_MESSAGE(ckp::load_slices({a.string()}, copies, err), err);
        CHECK(copies[0].fingerprint == cfg_for(a, o, 2).fingerprint);

        count.calls = 0;
        REQUIRE(ckp::run(cfg_for(a, o, 2), count.runner(), merged, err) == ckp::Outcome::Complete);
        CHECK(count.calls == 2); // both copies redone, not merged under o
        check_same(merged, full);
        std::error_code ec;
        fs::remove_all(a, ec);
        fs::remove_all(b, ec);
    }
}

TEST_CASE("a non-empty DIR without run.json is refused unless forced; other *.tmp stay") {
    const collatz::verify::Options o = small_opts(20);
    Counting count;
    std::string err;
    collatz::verify::Result merged;
    const fs::path d = tmpdir("foreign");
    fs::create_directories(d);
    std::ofstream(d / "notes.txt") << "mine\n";
    std::ofstream(d / "report.docx.tmp") << "mine\n";
    CHECK(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Error);
    CHECK_MESSAGE(err.find("--checkpoint-force") != std::string::npos, err);
    CHECK(count.calls == 0);
    CHECK_FALSE(fs::exists(d / "run.json"));

    ckp::RunConfig forced = cfg_for(d, o, 2);
    forced.force = true;
    REQUIRE(ckp::run(forced, count.runner(), merged, err) == ckp::Outcome::Complete);
    // A resume in the same DIR (run.json now present) keeps the user's files
    // and removes only the temp name this program writes.
    std::ofstream(d / (ckp::slice_file_name(1, 2) + ".tmp")) << "{\"sche";
    std::ofstream(d / "draft.tmp") << "mine\n";
    REQUIRE(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Complete);
    CHECK_FALSE(fs::exists(d / (ckp::slice_file_name(1, 2) + ".tmp")));
    CHECK(fs::exists(d / "notes.txt"));
    CHECK(fs::exists(d / "report.docx.tmp"));
    CHECK(fs::exists(d / "draft.tmp"));
    check_same(merged, collatz::verify::run(o));

    // A DIR holding only checkpoint files (slices copied without run.json) is
    // not foreign: its slices are used.
    fs::remove(d / "run.json");
    fs::remove(d / "notes.txt");
    fs::remove(d / "report.docx.tmp");
    fs::remove(d / "draft.tmp");
    count.calls = 0;
    REQUIRE(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Complete);
    CHECK(count.calls == 0);
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("a PAUSE that cannot be removed is an error, not a pause at every start") {
    const collatz::verify::Options o = small_opts(20);
    Counting count;
    std::string err;
    collatz::verify::Result merged;
    const fs::path d = tmpdir("pause");
    REQUIRE(ckp::run(cfg_for(d, o, 4), count.runner(), merged, err) == ckp::Outcome::Complete);
    fs::remove(d / ckp::slice_file_name(3, 4));
    // A non-empty directory named PAUSE: fs::remove fails on it everywhere.
    fs::create_directories(d / "PAUSE");
    std::ofstream(d / "PAUSE" / "x") << "x";
    ckp::pause_flag().store(0);
    count.calls = 0;
    CHECK(ckp::run(cfg_for(d, o, 4), count.runner(), merged, err) == ckp::Outcome::Error);
    CHECK_MESSAGE(err.find("cannot remove") != std::string::npos, err);
    CHECK(count.calls == 0);
    fs::remove_all(d / "PAUSE");
    REQUIRE(ckp::run(cfg_for(d, o, 4), count.runner(), merged, err) == ckp::Outcome::Complete);
    CHECK(count.calls == 1);
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("write_file_atomic leaves no temp file when the rename fails") {
    const fs::path d = tmpdir("atomic");
    fs::create_directories(d / "target");
    std::ofstream(d / "target" / "x") << "x"; // a non-empty directory: rename fails
    std::string err;
    CHECK_FALSE(ckp::write_file_atomic((d / "target").string(), "{}\n", err));
    CHECK_MESSAGE(err.find("rename") != std::string::npos, err);
    CHECK_FALSE(fs::exists(d / "target.tmp"));
    CHECK(fs::is_directory(d / "target"));
    REQUIRE(ckp::write_file_atomic((d / "ok.json").string(), "{}\n", err));
    CHECK(read_text(d / "ok.json") == "{}\n");
    CHECK_FALSE(fs::exists(d / "ok.json.tmp"));
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("a slice file above 256 MiB is refused unread by merge and resume") {
    const collatz::verify::Options o = small_opts(20);
    Counting count;
    std::string err;
    collatz::verify::Result merged;
    const fs::path d = tmpdir("big");
    REQUIRE(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Complete);
    fs::remove(d / "merged.json");
    const fs::path big = d / ckp::slice_file_name(1, 2);
    fs::resize_file(big, (std::uintmax_t{256} << 20) + 1); // sparse where supported
    ckp::Slice s;
    CHECK_FALSE(ckp::read_slice_file(big.string(), s, err));
    CHECK_MESSAGE(err.find("limit") != std::string::npos, err);
    std::vector<ckp::Slice> loaded;
    CHECK_FALSE(ckp::load_slices({d.string()}, loaded, err));
    count.calls = 0;
    CHECK(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Error);
    CHECK_MESSAGE(err.find(ckp::slice_file_name(1, 2)) != std::string::npos, err);
    CHECK(count.calls == 0);
    CHECK(fs::file_size(big) == (std::uintmax_t{256} << 20) + 1); // left as it was
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("a directory as a slice or run.json is refused unread") {
    const collatz::verify::Options o = small_opts(20);
    Counting count;
    std::string err;
    collatz::verify::Result merged;
    const fs::path d = tmpdir("special");
    REQUIRE(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Complete);
    const fs::path slice1 = d / ckp::slice_file_name(1, 2);
    fs::remove(slice1);
    fs::create_directory(slice1);
    ckp::Slice s;
    CHECK_FALSE(ckp::read_slice_file(slice1.string(), s, err));
    CHECK_MESSAGE(err.find("not a regular file") != std::string::npos, err);
    std::vector<ckp::Slice> loaded;
    CHECK_FALSE(ckp::load_slices({d.string()}, loaded, err));
    CHECK(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Error);
    CHECK_MESSAGE(err.find("not a regular file") != std::string::npos, err);
    fs::remove(slice1);
    fs::remove(d / "run.json");
    fs::create_directory(d / "run.json");
    CHECK(ckp::run(cfg_for(d, o, 2), count.runner(), merged, err) == ckp::Outcome::Error);
    CHECK_MESSAGE(err.find("run.json") != std::string::npos, err);
    std::error_code ec;
    fs::remove_all(d, ec);
}
