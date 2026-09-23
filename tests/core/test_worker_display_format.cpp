// The block progress and the text formatting of `collatz worker`'s display
// (collatz/worker_display.hpp): the bar of a run whose Step 1 runs ahead of the
// device, the block fraction / speed / ETA math with resumed slices, and the
// durations, counts, block labels, widths and bars.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/worker_display.hpp"
#include "progress_lines.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <vector>

namespace pg = collatz::progress;
namespace wk = collatz::worker;
using collatz::test::snap;
using collatz::test::written;

TEST_CASE("block progress: the bar follows the device when Step 1 runs ahead of it") {
    // A slice of 5000 tasks in four classes. Heavier tasks are more frequent
    // towards the end, as in the DFS order, and within a class later tasks
    // find up to 10% more. Each step Step 1 hands on whole tasks while the
    // device is less than 26% behind (what the producers and the queue hold),
    // the device finishes 1% of the survivors, and the worker reads the line.
    constexpr std::size_t kTasks = 5000;
    const double base[4] = {400, 1300, 2400, 3300};
    std::vector<unsigned> cls(kTasks);
    std::vector<std::uint64_t> size(kTasks);
    std::array<std::uint64_t, pg::kTaskClasses> per_class{};
    std::uint64_t total = 0;
    std::uint32_t x = 12345;
    for (std::size_t i = 0; i < kTasks; ++i) {
        x = x * 1664525u + 1013904223u;
        const double u = static_cast<double>(x >> 8) / 16777216.0;
        const double at = static_cast<double>(i) / kTasks;
        const double heavy = 0.05 + 0.15 * at; // the heaviest class: 5% at first, 20% at last
        cls[i] = u < heavy ? 3u : u < heavy + 0.25 ? 2u : u < heavy + 0.6 ? 1u : 0u;
        size[i] = static_cast<std::uint64_t>(base[cls[i]] * (1.0 + 0.1 * at));
        ++per_class[12 + cls[i]];
        total += size[i];
    }
    pg::set_slices(0, 0, 0);
    pg::start_tasks(per_class);

    const auto ahead = static_cast<std::uint64_t>(0.26 * static_cast<double>(total));
    const std::uint64_t rate = total / 100;
    std::size_t next = 0;    // tasks Step 1 has finished
    std::uint64_t found = 0; // survivors it handed on
    std::uint64_t done = 0;  // survivors the device finished
    double shown = 0.0;
    double worst = 0.0;
    for (int step = 1; done < total; ++step) {
        REQUIRE(step < 1000);
        CAPTURE(step);
        while (next < kTasks && found + size[next] <= done + ahead) {
            found += size[next];
            pg::survivors_found(size[next]);
            pg::task_finished(12 + cls[next], size[next]);
            ++next;
        }
        const std::uint64_t now = std::min(found, done + rate);
        pg::survivors_done(now - done);
        done = now;

        const pg::Snapshot s = written();
        const wk::BlockProgress p = wk::block_progress(s, 40, static_cast<double>(step), shown);
        const double truth = static_cast<double>(done) / static_cast<double>(total);
        CHECK(p.fraction >= shown); // never decreases
        worst = std::max(worst, std::abs(p.fraction - truth));
        if (step == 1) {
            // The device has finished 1% and Step 1 more than a quarter of
            // the tasks; the bar shows the device's share.
            CHECK(truth == doctest::Approx(0.01));
            CHECK(static_cast<double>(s.tasks_done) > 0.26 * static_cast<double>(s.tasks_total));
            CHECK(p.fraction < 0.02);
        }
        if (next == kTasks) {
            // Step 1 is done: the total is exact, and so is the bar once the
            // device has passed what it showed before.
            CHECK(s.survivors_total == total);
            CHECK(pg::slice_fraction(s) == doctest::Approx(truth));
            CHECK(p.fraction == doctest::Approx(std::max(truth, shown)));
        }
        shown = p.fraction;
    }
    CHECK(worst < 0.02);
    CHECK(shown == 1.0);
    pg::set_slices(0, 0, 0);
}

TEST_CASE(
    "block progress: fraction over slices and tasks; resumed slices are not this session's speed") {
    const double n40 = std::ldexp(1.0, 40);
    // Half of the one slice of a run without --checkpoint, in 10 s.
    wk::BlockProgress p = wk::block_progress(snap(50, 100, 0, 1, 0, 50, 100), 40, 10.0);
    CHECK(p.fraction == doctest::Approx(0.5));
    CHECK(p.session_numbers == doctest::Approx(0.5 * n40));
    CHECK(p.speed == doctest::Approx(0.5 * n40 / 10.0));
    CHECK(p.eta_s == doctest::Approx(10.0));

    // One of four slices done, half of the second.
    p = wk::block_progress(snap(30, 60, 1, 4, 0, 30, 60), 40, 15.0);
    CHECK(p.fraction == doctest::Approx(0.375));
    CHECK(p.eta_s == doctest::Approx(15.0 * 0.625 / 0.375));

    // Two slices were on disk at the start: the fraction counts them, the
    // speed does not. 3 of 4 done after 100 s means 1/4 of the block in 100 s.
    p = wk::block_progress(snap(0, 0, 3, 4, 2, 0, 0), 40, 100.0);
    CHECK(p.fraction == doctest::Approx(0.75));
    CHECK(p.session_numbers == doctest::Approx(0.25 * n40));
    CHECK(p.speed == doctest::Approx(0.25 * n40 / 100.0));
    CHECK(p.eta_s == doctest::Approx(100.0)); // the last quarter at the same speed

    // A finished run keeps its last slice's task counts: still 100%, not 125%.
    p = wk::block_progress(snap(5, 5, 4, 4, 0, 5, 5), 40, 50.0);
    CHECK(p.fraction == doctest::Approx(1.0));
    CHECK(p.eta_s == doctest::Approx(0.0));

    // Everything was resumed, or no time has passed: speed and ETA unknown.
    p = wk::block_progress(snap(0, 0, 4, 4, 4, 0, 0), 40, 50.0);
    CHECK(p.session_numbers == doctest::Approx(0.0));
    CHECK(p.speed == 0.0);
    CHECK(p.eta_s < 0.0);
    p = wk::block_progress(snap(1, 2, 0, 1, 0, 1, 2), 40, 0.0);
    CHECK(p.speed == 0.0);
    CHECK(p.eta_s < 0.0);
    // A slice whose prelude has not run counts as 0 of it.
    p = wk::block_progress(snap(0, 0, 1, 2, 0, 0, 0), 60, 5.0);
    CHECK(p.fraction == doctest::Approx(0.5));

    // The slice's share is its finished survivors, whatever the task count
    // says: one of four slices and a fifth of the next.
    p = wk::block_progress(snap(55, 60, 1, 4, 0, 200, 1000), 40, 10.0);
    CHECK(p.fraction == doctest::Approx(0.3));
    // A slice whose tasks finished without a survivor is done.
    p = wk::block_progress(snap(60, 60, 1, 4, 0, 0, 0), 40, 10.0);
    CHECK(p.fraction == doctest::Approx(0.5));
    // Before its first task finishes, nothing of it is known.
    p = wk::block_progress(snap(0, 60, 1, 4, 0, 0, 0), 40, 10.0);
    CHECK(p.fraction == doctest::Approx(0.25));
    // The bar does not go below what it showed: a rise in the estimated total
    // holds it, and so do the speed and ETA derived from it.
    p = wk::block_progress(snap(30, 60, 0, 1, 0, 400, 1000), 40, 10.0, 0.5);
    CHECK(p.fraction == doctest::Approx(0.5));
    CHECK(p.eta_s == doctest::Approx(10.0));
    // A finished run keeps its last slice's counts: still 100%.
    p = wk::block_progress(snap(60, 60, 4, 4, 0, 1000, 1000), 40, 50.0);
    CHECK(p.fraction == doctest::Approx(1.0));
}

TEST_CASE("formatting: durations, counts, block labels, widths and bars") {
    CHECK(wk::format_duration(0) == "0s");
    CHECK(wk::format_duration(42) == "42s");
    CHECK(wk::format_duration(8 * 60 + 12) == "8m 12s");
    CHECK(wk::format_duration(8 * 60 + 5) == "8m 05s");
    CHECK(wk::format_duration(3 * 3600 + 12 * 60 + 5) == "3h 12m");
    CHECK(wk::format_duration(2 * 86400 + 4 * 3600) == "2d 04h");
    CHECK(wk::format_duration(-1) == "--");
    CHECK(wk::format_duration(std::nan("")) == "--");

    CHECK(wk::format_count(0) == "0");
    CHECK(wk::format_count(9999) == "9999");
    CHECK(wk::format_count(1.52e13) == "1.52e13");
    CHECK(wk::format_count(2.52e17) == "2.52e17");
    CHECK(wk::format_count(9.996e12) == "1.00e13");
    CHECK(wk::format_count(-1) == "--");
    CHECK(wk::format_count(std::numeric_limits<double>::infinity()) == "--");

    // H = 2^17 + 37 with K = 54 starts at 2^71 + 37 * 2^54.
    CHECK(wk::block_label("131109", 54) == "2^71 + 37 * 2^54");
    CHECK(wk::block_label("131072", 54) == "2^71");
    CHECK(wk::block_label("1", 50) == "2^50");
    CHECK(wk::block_label("0", 40) == "0");
    CHECK(wk::block_label("x", 40) == "x * 2^40");
    CHECK(wk::power_offset_label("2361183241434822606848") == "2^71");
    CHECK(wk::power_offset_label("2361183241456297443328") == "2^71 + 2.15e10"); // + 5 * 2^32
    CHECK(wk::power_offset_label("junk") == "junk");

    CHECK(wk::fit_width("abcdef", 6) == "abcdef");
    CHECK(wk::fit_width("abcdef", 5) == "ab...");
    CHECK(wk::fit_width("abcdef", 2) == "ab");
    const std::string blocks = wk::progress_bar(1.0, 10, true); // ten 3-byte characters
    CHECK(blocks.size() == 30);
    CHECK(wk::columns(blocks) == 10);
    CHECK(wk::columns(wk::fit_width(blocks, 5)) == 5);
    CHECK(wk::fit_width(blocks, 5).substr(6) == "...");

    CHECK(wk::progress_bar(0.5, 10, false) == "[#####-----]");
    CHECK(wk::progress_bar(0.999, 10, false) == "[#########-]");
    CHECK(wk::progress_bar(1.0, 10, false) == "[##########]");
    CHECK(wk::progress_bar(-3.0, 4, false) == "[----]");
    CHECK(wk::progress_bar(std::nan(""), 4, false) == "[----]");
    const std::string half = wk::progress_bar(0.5, 10, true);
    CHECK(wk::columns(half) == 10);
    CHECK(half.substr(0, 3) == "\xE2\x96\x88"); // full block
    CHECK(half.substr(27) == "\xE2\x96\x91");   // light shade
    // 1/16 of 2 cells is one eighth of the first: U+258F, then shade.
    CHECK(wk::progress_bar(1.0 / 16.0, 2, true) == "\xE2\x96\x8F\xE2\x96\x91");
}
