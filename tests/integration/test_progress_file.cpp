// The real `verify --progress-file` at N=36 on the CPU and with --gpu: the file
// parses, and the results equal the pins.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/progress.hpp"
#include "collatz/worker.hpp"
#include "progress_lines.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace pg = collatz::progress;
namespace wk = collatz::worker;
namespace fs = std::filesystem;
using collatz::test::read_text;

#if defined(COLLATZ_BINARY)

namespace {

/// Runs `collatz verify ARGS` with its output in `log`; returns the exit code.
int run_verify(const std::vector<std::string>& args, const fs::path& log) {
    std::vector<std::string> argv = {COLLATZ_BINARY, "verify"};
    argv.insert(argv.end(), args.begin(), args.end());
    wk::ProcessOptions po;
    po.stdout_path = log.string();
    po.stderr_path = log.string();
    std::string err;
    const int rc = wk::run_process(argv, po, err);
    INFO(err);
    return rc;
}

/// The N=36 pins of the CLI defaults (also in tests/integration/test_frontier_blocks.cpp),
/// CPU and GPU alike.
void check_pins(const fs::path& json) {
    const std::optional<ckp::Json> doc = ckp::parse_json(read_text(json));
    REQUIRE(doc.has_value());
    const ckp::Json* r = doc->get("result");
    REQUIRE(r != nullptr);
    const auto text = [r](const char* k) {
        const ckp::Json* v = r->get(k);
        return v != nullptr ? v->text : std::string("<missing>");
    };
    CHECK(text("candidates_checked") == "71141351");
    CHECK(text("coverage_checksum") == "1012906364919380615");
    CHECK(text("pow_checksum") == "815100471492106042");
    CHECK(text("blocks_total") == "256500920");
    CHECK(text("max_excursion") == "102868194685920926084");
    CHECK(text("max_excursion_n") == "59436135663");
}

void progress_file_run(const std::vector<std::string>& backend) {
    const fs::path dir = fs::temp_directory_path() / ("collatz-progress-cli-" + wk::random_hex(6));
    fs::create_directories(dir);
    const auto with = [&backend](std::vector<std::string> a) {
        a.insert(a.end(), backend.begin(), backend.end());
        return a;
    };
    pg::Snapshot s;

    // A whole run: every split task finished, no checkpoint slices.
    REQUIRE(run_verify(with({"--N",
                             "36",
                             "--progress-file",
                             (dir / "p1.txt").string(),
                             "--json",
                             (dir / "r1.json").string()}),
                       dir / "v1.log") == 0);
    REQUIRE(pg::parse_line(read_text(dir / "p1.txt"), s));
    CHECK(s.tasks_total > 0);
    CHECK(s.tasks_done == s.tasks_total);
    CHECK(s.slices_done == 0);
    CHECK(s.slices_total == 1);
    CHECK(s.survivors_total > 0);
    CHECK(s.survivors_done == s.survivors_total);
    check_pins(dir / "r1.json");

    // Four checkpoint slices, then a resume that finds all four on disk.
    const fs::path ck = dir / "ck";
    for (int round = 0; round < 2; ++round) {
        CAPTURE(round);
        const std::string p = (dir / ("p" + std::to_string(round + 2) + ".txt")).string();
        const std::string j = (dir / ("r" + std::to_string(round + 2) + ".json")).string();
        REQUIRE(run_verify(with({"--N",
                                 "36",
                                 "--checkpoint",
                                 ck.string(),
                                 "--slices",
                                 "4",
                                 "--progress-file",
                                 p,
                                 "--json",
                                 j}),
                           dir / "v2.log") == 0);
        REQUIRE(pg::parse_line(read_text(p), s));
        CHECK(s.slices_done == 4);
        CHECK(s.slices_total == 4);
        CHECK(s.slices_resumed == (round == 0 ? 0u : 4u));
        check_pins(j);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

TEST_CASE("verify --progress-file on the CPU: a parseable file, the N=36 pins unchanged") {
    progress_file_run({"--threads", "4"});
}

TEST_CASE("verify --progress-file with --gpu: a parseable file, the N=36 pins unchanged") {
    // Without a device (or OpenCL) verify falls back to the CPU; the pins hold either way.
    progress_file_run({"--gpu"});
}

#endif
