// The verify progress file (collatz/progress.hpp): the line format, the
// counters and their file writer, and the survivor estimate.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/progress.hpp"
#include "collatz/worker.hpp"
#include "progress_lines.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>

namespace pg = collatz::progress;
namespace wk = collatz::worker;
namespace fs = std::filesystem;
using collatz::test::read_text;
using collatz::test::snap;
using collatz::test::written;

TEST_CASE("progress line: format and parse round-trip; malformed lines are refused") {
    const pg::Snapshot s = snap(37, 5312, 3, 256, 1, 812, 90210);
    const std::string line = pg::format_line(s);
    CHECK(line == "37 5312 3 256 1 812 90210\n");
    pg::Snapshot back;
    REQUIRE(pg::parse_line(line, back));
    CHECK(back.tasks_done == 37);
    CHECK(back.tasks_total == 5312);
    CHECK(back.slices_done == 3);
    CHECK(back.slices_total == 256);
    CHECK(back.slices_resumed == 1);
    CHECK(back.survivors_done == 812);
    CHECK(back.survivors_total == 90210);
    REQUIRE(pg::parse_line("37 5312 3 256 1 812 90210\r\n", back)); // a Windows editor's line end
    CHECK(back.survivors_total == 90210);
    REQUIRE(pg::parse_line("18446744073709551615 18446744073709551615 0 1 0 0 0", back));
    CHECK(back.tasks_total == std::numeric_limits<std::uint64_t>::max());

    for (const char* bad : {"",
                            "\n",
                            "7",
                            "1 2 3",
                            "1 2 3 4",
                            "1 2 3 4 0 6",
                            "a 2 0 1 0 0 0",
                            "-1 2 0 1 0 0 0",
                            "+1 2 0 1 0 0 0",
                            "1  2 0 1 0 0 0",
                            " 1 2 0 1 0 0 0",
                            "1 2 0 1 0 0 0 ",
                            "1\t2 0 1 0 0 0",
                            "11 10 0 1 0 0 0",                  // done above total
                            "1 2 0 0 0 0 0",                    // no slices
                            "1 2 5 4 0 0 0",                    // slices done above total
                            "1 2 2 4 3 0 0",                    // more resumed than done
                            "1 2 0 1 0 5",                      // six fields
                            "1 2 0 1 0 5 4",                    // survivors done above total
                            "1 2 0 1 0 1 2 3",                  // eight fields
                            "18446744073709551616 1 0 1 0 0 0", // 2^64
                            "1 2 0 1 0 0 0\n3 4 0 1 0 0 0"}) {
        CAPTURE(bad);
        pg::Snapshot out = snap(9, 9, 9, 9, 9, 9, 9);
        CHECK_FALSE(pg::parse_line(bad, out));
        CHECK(out.tasks_done == 9); // untouched
    }
}

TEST_CASE("progress counters: snapshot clamps and the writer leaves a parseable file") {
    pg::set_slices(0, 0, 0);                   // no checkpoint layer
    const std::uint64_t five[] = {0, 0, 0, 5}; // five tasks of class 3
    pg::start_tasks(five);
    for (int i = 0; i < 7; ++i) {
        pg::task_finished(3, 0); // more finished than started
    }
    pg::survivors_done(4); // and finished survivors none of them found
    pg::Snapshot s = pg::snapshot();
    CHECK(s.tasks_done == 5);
    CHECK(s.tasks_total == 5);
    CHECK(s.slices_done == 0);
    CHECK(s.slices_total == 1);
    CHECK(s.slices_resumed == 0);
    CHECK(s.survivors_done == 0);
    CHECK(s.survivors_total == 0);
    CHECK(pg::slice_fraction(s) == 1.0); // every task finished, none found a survivor
    pg::set_slices(2, 8, 1);
    CHECK(pg::format_line(pg::snapshot()) == "0 0 2 8 1 0 0\n"); // the next slice has no tasks yet
    const std::uint64_t forty[] = {0, 40};
    pg::start_tasks(forty);
    pg::survivors_found(100);
    pg::survivors_done(30);
    CHECK(pg::format_line(pg::snapshot()) == "0 40 2 8 1 0 0\n"); // no task finished: no estimate
    CHECK(pg::slice_fraction(pg::snapshot()) == 0.0);
    pg::task_finished(1, 60);
    pg::task_finished(1, 40);
    // 2 of 40 tasks found 50 survivors each, so the slice has about 2000.
    s = pg::snapshot();
    CHECK(pg::format_line(s) == "2 40 2 8 1 30 2000\n");
    CHECK(pg::slice_fraction(s) == doctest::Approx(0.015));
    const pg::SurvivorCounts c = pg::survivor_counts();
    CHECK(c.found == 100);
    CHECK(c.done == 30);
    pg::set_slices(3, 8, 1); // its tasks and survivors are gone in the same step
    CHECK(pg::format_line(pg::snapshot()) == "0 0 3 8 1 0 0\n");
    CHECK(pg::survivor_counts().found == 0);

    const fs::path dir = fs::temp_directory_path() / ("collatz-progress-" + wk::random_hex(6));
    fs::create_directories(dir);
    const fs::path file = dir / "progress.txt";
    REQUIRE(pg::write_line(file, "1 2 0 1 0\n"));
    CHECK(read_text(file) == "1 2 0 1 0\n");
    {
        const pg::FileWriter w(file, std::chrono::milliseconds(20));
        const std::uint64_t three[] = {3};
        pg::start_tasks(three);
        pg::survivors_found(10);
        pg::task_finished(0, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pg::Snapshot mid;
        CHECK(pg::parse_line(read_text(file), mid));
        pg::survivors_found(14);
        pg::task_finished(0, 6);
        pg::task_finished(0, 8);
        pg::survivors_done(24);
    }
    pg::Snapshot last;
    REQUIRE(pg::parse_line(read_text(file), last)); // the destructor's final write
    CHECK(last.tasks_done == 3);
    CHECK(last.tasks_total == 3);
    CHECK(last.survivors_done == 24);
    CHECK(last.survivors_total == 24);
    CHECK_FALSE(fs::exists(dir / "progress.txt.tmp"));
    pg::set_slices(0, 0, 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("survivor estimate: class by class, never below what was found, exact at the end") {
    pg::set_slices(0, 0, 0);
    // 60 light tasks (f=12, 100 survivors each) and 40 heavy ones (f=15,
    // 1000 each): 46000 survivors.
    std::array<std::uint64_t, pg::kTaskClasses> per_class{};
    per_class[12] = 60;
    per_class[15] = 40;
    pg::start_tasks(per_class);

    // The first two tasks to finish found nothing, while the running ones
    // have handed on 4096 survivors. The class averages say 0; the ratio of
    // found survivors to finished tasks does not.
    for (int i = 0; i < 2; ++i) {
        pg::task_finished(12, 0);
    }
    pg::survivors_found(4096);
    pg::survivors_done(4096);
    pg::Snapshot s = written();
    CHECK(s.survivors_total == 4096 * 100 / 2);
    CHECK(pg::slice_fraction(s) == doctest::Approx(0.02));

    // 30 light and 4 heavy tasks finished (34% of the tasks, 7000 survivors)
    // plus 500 from the running ones: the class averages give the whole slice,
    // where the task count would claim a third of it.
    pg::start_tasks(per_class);
    for (int i = 0; i < 30; ++i) {
        pg::task_finished(12, 100);
    }
    for (int i = 0; i < 4; ++i) {
        pg::task_finished(15, 1000);
    }
    pg::survivors_found(7500);
    pg::survivors_done(7500);
    s = written();
    CHECK(s.survivors_total == 46000);
    CHECK(pg::slice_fraction(s) == doctest::Approx(7500.0 / 46000.0));
    CHECK(static_cast<double>(s.tasks_done) / static_cast<double>(s.tasks_total) ==
          doctest::Approx(0.34));

    // A class with no finished task counts at the average of all finished
    // ones: 10 light tasks found 100 each, so all 110 tasks count 100.
    per_class[17] = 10;
    pg::start_tasks(per_class);
    for (int i = 0; i < 10; ++i) {
        pg::task_finished(12, 100);
    }
    pg::survivors_found(1000);
    CHECK(written().survivors_total == 11000);

    // Every task finished: the total is exactly what they found.
    pg::start_tasks(per_class);
    std::uint64_t all = 0;
    for (int i = 0; i < 60; ++i) {
        pg::task_finished(12, 90 + static_cast<std::uint64_t>(i % 3) * 10);
        all += 90 + static_cast<std::uint64_t>(i % 3) * 10;
    }
    for (int i = 0; i < 40; ++i) {
        pg::task_finished(15, 1000);
        all += 1000;
    }
    for (int i = 0; i < 10; ++i) {
        pg::task_finished(17, 3000);
        all += 3000;
    }
    pg::survivors_found(all);
    pg::survivors_done(all - 1000); // the last batches still on the device
    s = written();
    CHECK(s.tasks_done == s.tasks_total);
    CHECK(s.survivors_total == all);
    CHECK(s.survivors_done == all - 1000);
    pg::survivors_done(1000);
    CHECK(pg::slice_fraction(written()) == 1.0);
    pg::set_slices(0, 0, 0);
}
