// The run fingerprint (src/core/checkpoint.cpp): what it records and ignores,
// the classification of every COLLATZ_* env name, and the block H and depth K
// in the fingerprint and on resume.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "block_mode.hpp"
#include "checkpoint_helpers.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/verify.hpp"

#include <atomic>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace ckp = collatz::checkpoint;
namespace fs = std::filesystem;
namespace v = collatz::verify;
using collatz::test::block_opts;
using collatz::test::small_opts;

namespace {

// COLLATZ_* names that cannot change a printed Result scalar: batch and chunk
// sizes, memory caps, caches and file I/O, and the settings that only
// `collatz worker` reads. They are not part of the fingerprint, so they may
// change between sessions.
const char* const kShapeEnv[] = {
    "COLLATZ_ASCII",
    "COLLATZ_CACHE_DIR",
    "COLLATZ_DIP_CACHE",
    "COLLATZ_GPU_CPI_CAND_CAP",
    "COLLATZ_GPU_DEVICE",
    "COLLATZ_GPU_EARLY_UPLOAD_CHUNK_MB",
    "COLLATZ_GPU_FLUSH_CAP",
    "COLLATZ_GPU_GLOBAL_MEM",
    "COLLATZ_GPU_LOCAL_MEM",
    "COLLATZ_GPU_MAX_ALLOC",
    "COLLATZ_MMAP_WILLNEED",
    "COLLATZ_WORKER_BACKOFF_MS",
    "COLLATZ_WORKER_TOKEN",
};

} // namespace

TEST_CASE("fingerprint ignores threads, records result-affecting options") {
    collatz::verify::Options a = small_opts(30);
    collatz::verify::Options b = a;
    b.threads = 7;
    CHECK(ckp::make_fingerprint(a, false, "x") == ckp::make_fingerprint(b, false, "x"));
    b.B = a.B + 1;
    const auto d = ckp::fingerprint_diff(ckp::make_fingerprint(a, false, "x"),
                                         ckp::make_fingerprint(b, false, "x"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rfind("B:", 0) == 0);
    CHECK_FALSE(ckp::make_fingerprint(a, false, "x") ==
                ckp::make_fingerprint(a, false, "y")); // commit
}

TEST_CASE("S2: every COLLATZ_* env name in src/ and include/ is classified") {
    // Scan every quoted "COLLATZ_..." literal (getenv and the env_flag-style
    // helpers that wrap it). checkpoint.cpp holds the result list itself.
    const fs::path root = COLLATZ_SOURCE_DIR;
    REQUIRE(fs::is_directory(root / "src"));
    const std::regex lit("\"(COLLATZ_[A-Z0-9_]+)\"");
    std::set<std::string> used;
    for (const char* sub : {"src", "include"}) {
        for (const auto& e : fs::recursive_directory_iterator(root / sub)) {
            const std::string ext = e.path().extension().string();
            if (!e.is_regular_file() ||
                (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") ||
                e.path().filename() == "checkpoint.cpp") {
                continue;
            }
            std::ifstream in(e.path());
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            for (std::sregex_iterator it(text.begin(), text.end(), lit), end; it != end; ++it) {
                used.insert((*it)[1].str());
            }
        }
    }
    REQUIRE(used.count("COLLATZ_CACHE_DIR") == 1); // the scan found the sources
    std::set<std::string> result(ckp::result_env_names().begin(), ckp::result_env_names().end());
    const std::set<std::string> shape(std::begin(kShapeEnv), std::end(kShapeEnv));
    for (const std::string& n : used) {
        CHECK_MESSAGE((result.count(n) + shape.count(n)) == 1,
                      n << ": add it to kResultEnv in src/core/checkpoint.cpp (can change a "
                           "printed result; the default when unsure) or to kShapeEnv here");
    }
    for (const std::string& n : result) {
        CHECK_MESSAGE(shape.count(n) == 0, n << " is in both lists");
        CHECK_MESSAGE(used.count(n) == 1, n << " is listed but no longer read");
    }
    for (const std::string& n : shape) {
        CHECK_MESSAGE(used.count(n) == 1, n << " is listed but no longer read");
    }
}

TEST_CASE("checkpoint fingerprint and resume refuse a different block H or depth K") {
    const v::Options o1 = block_opts(32, 5);
    v::Options o2 = o1;
    o2.block_h = 6;
    v::Options o3 = o1;
    o3.N = 33;
    const ckp::Fingerprint f1 = ckp::make_fingerprint(o1, false, "c0ffee");
    const ckp::Fingerprint f2 = ckp::make_fingerprint(o2, false, "c0ffee");
    const ckp::Fingerprint f3 = ckp::make_fingerprint(o3, false, "c0ffee");
    CHECK(f1.at("block_h") == "5");
    const auto d12 = ckp::fingerprint_diff(f1, f2);
    REQUIRE(d12.size() == 1);
    CHECK(d12[0].find("block_h") != std::string::npos);
    const auto d13 = ckp::fingerprint_diff(f1, f3);
    REQUIRE(d13.size() == 1);
    CHECK(d13[0].find('N') == 0);

    // A real checkpoint directory, driven by a stub runner (no verification):
    // the first run finishes, a rerun with another H or K is refused.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("collatz_frontier_ckpt_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir);
    std::atomic<int> runs{0};
    const ckp::SliceRunner stub = [&runs](const v::Options&, std::string& be) {
        ++runs;
        be = "cpu";
        v::Result r;
        r.pow_available = true;
        r.pow_kernel_id = 16;
        r.candidates_checked = 1;
        r.max_excursion = 1000;
        r.max_excursion_n = 77;
        return r;
    };
    const auto make_cfg = [&](const v::Options& o) {
        ckp::RunConfig cfg;
        cfg.dir = dir.string();
        cfg.K = 2;
        cfg.K_explicit = true;
        cfg.opts = o;
        cfg.fingerprint = ckp::make_fingerprint(o, false, "c0ffee");
        return cfg;
    };
    v::Result merged;
    std::string err;
    REQUIRE(ckp::run(make_cfg(o1), stub, merged, err) == ckp::Outcome::Complete);
    CHECK(runs.load() == 2);
    CHECK(ckp::run(make_cfg(o1), stub, merged, err) == ckp::Outcome::Complete);
    CHECK(runs.load() == 2); // resumed: nothing re-run
    CHECK(ckp::run(make_cfg(o2), stub, merged, err) == ckp::Outcome::Error);
    CHECK(err.find("block_h") != std::string::npos);
    CHECK(ckp::run(make_cfg(o3), stub, merged, err) == ckp::Outcome::Error);
    CHECK(err.find("N:") != std::string::npos);
    CHECK(runs.load() == 2);
    std::filesystem::remove_all(dir);
}
