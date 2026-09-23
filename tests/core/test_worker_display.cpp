// `collatz worker`'s progress display (collatz/worker_display.hpp): the renderer
// and the plain line, the redraw escapes, the --status summary of /api/stats, and
// the terminal-name checks.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace wk = collatz::worker;

namespace {

bool ascii_only(const std::string& s) {
    for (const char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80 || c == '\x1b') {
            return false;
        }
    }
    return true;
}

wk::DisplayState running_state() {
    wk::DisplayState s;
    s.device = "GPU NVIDIA GeForce RTX 3070";
    s.uptime_s = 3 * 3600 + 12 * 60;
    s.server = "https://collatz-frontier.example.workers.dev";
    s.running = true;
    s.unit_id = 812;
    s.block = wk::block_label("131109", 54);
    s.block_bits = 54;
    s.started = "09:12";
    s.have_progress = true;
    s.progress.fraction = 0.583;
    s.progress.speed = 1.52e13;
    s.progress.eta_s = 8 * 60 + 12;
    s.session_blocks = 14;
    s.session_numbers = 2.52e17;
    s.session_since = "06:01";
    s.have_frontier = true;
    s.frontier_log2 = 71.0034;
    s.frontier_value = "2366754405160605712384"; // 2^71.0034
    s.frontier_age_s = 120;
    s.last = "unit 811 accepted at 09:11 after 19m 40s";
    s.hint = "Ctrl-C pauses (the block's progress is kept and resumed next time)";
    return s;
}

} // namespace

TEST_CASE("render: lines fit the width, ASCII and UTF-8 bars, no escape codes") {
    const wk::DisplayState s = running_state();
    const std::vector<std::string> ascii = wk::render(s, wk::RenderOptions{80, false});
    REQUIRE(ascii.size() == 9);
    CHECK(ascii[0] == "collatz worker   GPU NVIDIA GeForce RTX 3070   up 3h 12m");
    CHECK(ascii[1] == "server   https://collatz-frontier.example.workers.dev");
    CHECK(ascii[2] == "block    2^71 + 37 * 2^54  (2^54 numbers, unit 812, started 09:12)");
    CHECK(ascii[3].rfind("progress [####", 0) == 0);
    CHECK(ascii[3].find("]   58.3%  ETA 8m 12s") != std::string::npos);
    CHECK(ascii[4] == "speed    1.52e13 numbers/s this block, 2.19e13 this session");
    CHECK(ascii[5] == "session  14 blocks, 2.52e17 numbers (since 06:01)");
    CHECK(ascii[6] == "frontier 2^71 + 5.57e18 (2^71.0034), from the server 2 min ago");
    CHECK(ascii[7] == "last     unit 811 accepted at 09:11 after 19m 40s");
    CHECK(ascii[8] == s.hint);
    for (const std::string& l : ascii) {
        CAPTURE(l);
        CHECK(ascii_only(l));
        CHECK(wk::columns(l) <= 79);
    }

    const std::vector<std::string> utf8 = wk::render(s, wk::RenderOptions{80, true});
    REQUIRE(utf8.size() == ascii.size());
    CHECK(utf8[3].find("\xE2\x96\x88") != std::string::npos);
    CHECK(utf8[3].find('#') == std::string::npos);
    CHECK(utf8[3].find("58.3%") != std::string::npos);
    for (const std::string& l : utf8) {
        CHECK(l.find('\x1b') == std::string::npos);
        CHECK(wk::columns(l) <= 79);
    }

    // A narrow console: every line is cut so that none wraps.
    for (const bool u : {false, true}) {
        const std::vector<std::string> narrow = wk::render(s, wk::RenderOptions{40, u});
        REQUIRE(narrow.size() == ascii.size());
        for (const std::string& l : narrow) {
            CAPTURE(l);
            CHECK(wk::columns(l) <= 39);
        }
        CHECK(narrow[2].substr(narrow[2].size() - 3) == "...");
    }
    // A width below 24 counts as 24, so lines are cut to 23 columns, not to nothing.
    for (const std::string& l : wk::render(s, wk::RenderOptions{1, false})) {
        CHECK(wk::columns(l) <= 23);
    }

    // Before the first progress line, and while no block runs.
    wk::DisplayState st = s;
    st.have_progress = false;
    st.progress = wk::BlockProgress{};
    std::vector<std::string> lines = wk::render(st, wk::RenderOptions{80, false});
    CHECK(lines[3].find("starting") != std::string::npos);
    CHECK(lines[4].rfind("speed    -- this block", 0) == 0);
    st.running = false;
    st.status = "no work right now: busy; next try in 9m 40s";
    st.have_frontier = false;
    st.last.clear();
    lines = wk::render(st, wk::RenderOptions{80, false});
    REQUIRE(lines.size() == 7);
    CHECK(lines[2] == "status   no work right now: busy; next try in 9m 40s");
    CHECK(lines[3] == "speed    2.19e13 numbers/s this session");
    CHECK(lines[5] == "frontier not known yet");
}

TEST_CASE("plain progress line and the redraw escapes") {
    const wk::DisplayState s = running_state();
    const std::string line = wk::plain_progress_line(s);
    CHECK(line ==
          "worker: unit 812 58.3% done (2^54 numbers at 2^71 + 37 * 2^54), 1.52e13 numbers/s, "
          "ETA 8m 12s");
    CHECK(ascii_only(line));
    CHECK(line.rfind("unit ", 0) != 0); // the e2e tests count lines starting "unit <id> "
    wk::DisplayState idle = s;
    idle.running = false;
    idle.status = "waiting";
    CHECK(wk::plain_progress_line(idle) == "worker: waiting");

    const std::vector<std::string> lines = {"a", "b"};
    CHECK(wk::redraw(0, lines) == "\r\r\x1b[2Ka\n\r\x1b[2Kb\n\r\x1b[J");
    CHECK(wk::redraw(3, lines) == "\r\x1b[3A\r\x1b[2Ka\n\r\x1b[2Kb\n\r\x1b[J");
    CHECK(wk::erase(0).empty());
    CHECK(wk::erase(9) == "\r\x1b[9A\x1b[J");

    // A frame cut for 80 columns, after the window shrank to 40: a 79-column
    // line rewraps to 2 rows, a 40-column one stays 1, an empty one is 1.
    const std::vector<std::string> frame = {std::string(79, 'x'), std::string(40, 'y'), ""};
    CHECK(wk::screen_rows(frame, 80) == 3);
    CHECK(wk::screen_rows(frame, 120) == 3);
    CHECK(wk::screen_rows(frame, 40) == 4);
    CHECK(wk::screen_rows(frame, 20) == 4 + 2 + 1);
    CHECK(wk::screen_rows(frame, 0) == 3); // width unknown
}

TEST_CASE("--status: the /api/stats summary") {
    const std::string body =
        R"({"generated_at":1791450887,)"
        R"("campaign":{"id":1,"name":"frontier","start":"2361183241434822606848","start_log2":71},)"
        R"("frontier":{"value":"2361183262909659086848","log2":71.0000000131},)"
        R"("totals":{"numbers_verified":"21474836480","blocks":3,"last_block_at":1791450800},)"
        R"("speed":{"last_10_min":{"seconds":600,"blocks":3,"numbers_per_second":35791394.13},)"
        R"("last_hour":{"seconds":3600,"blocks":3,"numbers_per_second":5965232.35},)"
        R"("last_24_h":{"seconds":86400,"blocks":120,"numbers_per_second":1.52e13},)"
        R"("all_time":{"seconds":86400,"blocks":3,"numbers_per_second":248551.348},)"
        R"("blocks_per_day":120},)"
        R"("milestones":[],"history":{"hourly":[],"daily":[]},)"
        R"("hosts":[{"host":"0123456789abcdef0123456789abcdef","backend":"gpu","blocks":14,)"
        R"("numbers":"252000000000000000","numbers_per_second":1.48e13,"last_at":1791450707},)"
        R"({"host":"fedcba9876543210fedcba9876543210","backend":"cpu","blocks":1,)"
        R"("numbers":"17179869184","numbers_per_second":1402438290.5,"last_at":1791363000}],)"
        R"("blocks":[],"max_excursion":null,"alerts_open":0})";
    const std::optional<ckp::Json> doc = ckp::parse_json(body);
    REQUIRE(doc.has_value());
    const std::vector<std::string> lines = wk::stats_summary(*doc);
    REQUIRE(lines.size() == 7);
    CHECK(lines[0] == "campaign frontier (id 1), checking from 2^71.0000");
    CHECK(lines[1] == "frontier: 2361183262909659086848 (2^71.0000)");
    CHECK(lines[2] == "numbers checked: 21474836480 (2.15e10) in 3 blocks");
    CHECK(lines[3] == "speed (24 h): 1.52e13 numbers/s, 120 blocks a day");
    CHECK(lines[4] == "machines: 2");
    CHECK(lines[5] ==
          "  01234567  gpu  14 blocks, 2.52e17 numbers, 1.48e13 numbers/s, last block 3 min ago");
    CHECK(lines[6] ==
          "  fedcba98  cpu  1 block, 1.72e10 numbers, 1.40e9 numbers/s, last block 24 h ago");
    double lg = 0;
    std::string value;
    REQUIRE(wk::stats_frontier(*doc, lg, value));
    CHECK(lg == doctest::Approx(71.0000000131));
    CHECK(value == "2361183262909659086848");

    // No block in the last day, an open alert, and server text with a
    // terminal escape in it.
    const std::optional<ckp::Json> quiet = ckp::parse_json(
        R"({"campaign":{"id":2,"name":"x\u001b[31m"},"frontier":{"value":"5","log2":2.32},)"
        R"("totals":{"numbers_verified":"0","blocks":0},"speed":{"last_24_h":{"numbers_per_second":null}},)"
        R"("hosts":[],"alerts_open":2})");
    REQUIRE(quiet.has_value());
    const std::vector<std::string> q = wk::stats_summary(*quiet);
    REQUIRE(q.size() == 6);
    CHECK(q[0] == "campaign x?[31m (id 2)");
    CHECK(q[1] == "frontier: 5 (2^2.3200)");
    CHECK(q[2] == "numbers checked: 0 in 0 blocks");
    CHECK(q[3] == "speed (24 h): no block accepted in the last 24 hours");
    CHECK(q[4] == "machines: 0");
    CHECK(q[5] == "alerts: 2 open");

    const std::optional<ckp::Json> none =
        ckp::parse_json(R"({"campaign":null,"message":"no campaign has been created yet"})");
    REQUIRE(none.has_value());
    CHECK(wk::stats_summary(*none) == std::vector<std::string>{"no campaign has been created yet"});
    CHECK_FALSE(wk::stats_frontier(*none, lg, value));
}

TEST_CASE("terminal checks: the verify log's GPU line, MSYS2 pty names, UTF-8 locales") {
    CHECK(wk::gpu_device_from_log("note: --gpu without --threads\n"
                                  "gpu device: NVIDIA GeForce RTX 3070\r\nverify: N=50\n") ==
          "NVIDIA GeForce RTX 3070");
    CHECK(wk::gpu_device_from_log("gpu device: NVIDIA GeForce RTX 3070").empty()); // no newline yet
    CHECK(wk::gpu_device_from_log("note: --gpu without --threads\n").empty());
    CHECK(wk::gpu_device_from_log("gpu device: bad\x1b[2Jname\n") == "bad?[2Jname");

    CHECK(wk::is_msys_pty_name(L"\\msys-1888ae32e00d56aa-pty0-to-master"));
    CHECK(wk::is_msys_pty_name(L"\\cygwin-e022582115c10879-pty4-to-master"));
    CHECK(wk::is_msys_pty_name(L"\\Device\\NamedPipe\\msys-1888ae32e00d56aa-pty1-to-master"));
    CHECK_FALSE(wk::is_msys_pty_name(L"\\msys-1888ae32e00d56aa-pipe-0"));
    CHECK_FALSE(wk::is_msys_pty_name(L"\\some-pty-pipe"));
    CHECK_FALSE(wk::is_msys_pty_name(L""));

    CHECK(wk::locale_is_utf8(nullptr, nullptr, "en_US.UTF-8"));
    CHECK(wk::locale_is_utf8("", "de_DE.utf8", "C"));
    CHECK_FALSE(wk::locale_is_utf8("C", nullptr, "en_US.UTF-8")); // LC_ALL wins
    CHECK_FALSE(wk::locale_is_utf8(nullptr, nullptr, nullptr));
    CHECK_FALSE(wk::locale_is_utf8(nullptr, nullptr, "POSIX"));
}
