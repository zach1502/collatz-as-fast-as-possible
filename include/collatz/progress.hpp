#ifndef COLLATZ_PROGRESS_HPP
#define COLLATZ_PROGRESS_HPP

// Progress of a verify run for `verify --progress-file PATH`, which `collatz
// worker` reads to show how far a block is. The Step-1 enumeration counts its
// split tasks and the survivors it hands on, Step 2 and 3 (the CPU sink, the
// GPU consumer) count the survivors they have finished, and checkpoint::run
// counts its slices, all in process-wide atomics; a FileWriter thread copies
// them into PATH every few seconds. Writing the file is the only effect: no
// Result scalar depends on it.
//
// The file holds one line of seven decimal integers separated by single spaces
// (shown on two lines here):
//
//     tasks_done tasks_total slices_done slices_total slices_resumed
//     survivors_done survivors_total
//
// tasks_*: the Step-1 split tasks (subtrees at the split depth of step1.cpp)
// of the slice in progress, finished and in total; "0 0" until that slice's
// serial prelude has run. A serial (one-thread) enumeration is one task. A task
// counts as finished when its enumeration is done; on the GPU its survivors
// may still wait in the device queue.
// slices_*: the checkpoint slices of this run (its --slice-range), finished and
// in total, and how many of the finished ones were already on disk when this
// process started (a resumed run); "0 1 0" without --checkpoint.
// survivors_*: the Step-1 survivors of the slice in progress whose Step-2 mask
// and Step-3 tails are finished (on the GPU, whose device batch has been
// folded), and all survivors of the slice. Until every task has finished, the
// total is an estimate from the finished tasks (see snapshot); "0 0" until the
// first task finishes. survivors_done / survivors_total is the finished share
// of the slice's work, which the task count overstates while the device lags.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace collatz::progress {

/// Split tasks are grouped by the odd-step count f of their root for the
/// survivor estimate: task_class(f) is f, and every f at or above the last
/// class falls in it.
inline constexpr unsigned kTaskClasses = 64;
[[nodiscard]] constexpr unsigned task_class(unsigned f) noexcept {
    return f < kTaskClasses ? f : kTaskClasses - 1;
}

/// One progress line, parsed or about to be written.
struct Snapshot {
    std::uint64_t tasks_done = 0;
    std::uint64_t tasks_total = 0;
    std::uint64_t slices_done = 0;
    std::uint64_t slices_total = 1;
    std::uint64_t slices_resumed = 0;
    std::uint64_t survivors_done = 0;
    std::uint64_t survivors_total = 0;
};

/// Starts the split tasks of the slice in progress: `per_class[c]` tasks of
/// class c (task_class). Zeroes the finished tasks and the survivor counts.
void start_tasks(std::span<const std::uint64_t> per_class) noexcept;

/// Counts one finished split task of class `cls` (task_class) that found
/// `survivors` Step-1 survivors. Called once per task, never per survivor.
void task_finished(unsigned cls, std::uint64_t survivors) noexcept;

/// Counts `n` Step-1 survivors handed on to Step 2 and 3. Called once per
/// buffer of survivors.
void survivors_found(std::uint64_t n) noexcept;

/// Counts `n` survivors whose Step-2 and Step-3 work is finished. Called once
/// per batch, never per survivor or candidate.
void survivors_done(std::uint64_t n) noexcept;

/// The survivors of the slice in progress handed on by Step 1 and finished,
/// unclamped, for the tests that check every consumer counts each survivor
/// exactly once.
struct SurvivorCounts {
    std::uint64_t found = 0;
    std::uint64_t done = 0;
};
[[nodiscard]] SurvivorCounts survivor_counts() noexcept;

/// Sets the slice counts of a checkpointed run and zeroes the task and
/// survivor counts, in one step, so that a snapshot sees either the slice in
/// progress with its tasks or the next slice count with none.
void set_slices(std::uint64_t done, std::uint64_t total, std::uint64_t resumed) noexcept;

/// The counters as one consistent line, each count clamped to its total.
/// While tasks are unfinished, survivors_total is an estimate made class by
/// class: the unfinished tasks of a class are taken to find as many survivors
/// as its finished ones did on average (a class with none finished counts at
/// the average of all finished tasks). Survivors per task grow steeply with f
/// and the last tasks in DFS order have more odd steps, so one ratio of
/// survivors to tasks over all classes comes out low until the end. The
/// estimate is never below that ratio, which counts the running tasks'
/// survivors as if those tasks had finished and so is the larger one while few
/// tasks are done, nor below the survivors found.
/// Once every task has finished the total is exact.
[[nodiscard]] Snapshot snapshot() noexcept;

/// The finished share (0..1) of the slice in progress: survivors_done /
/// survivors_total. 1 for a slice whose tasks have all finished without a
/// survivor; 0 while nothing is known.
[[nodiscard]] double slice_fraction(const Snapshot& s) noexcept;

/// The file's line for `s`, with its newline.
[[nodiscard]] std::string format_line(const Snapshot& s);

/// Parses a line of format_line (a trailing newline or CR LF is allowed). False
/// for anything else, including counts above their totals.
[[nodiscard]] bool parse_line(std::string_view text, Snapshot& out);

/// Replaces `path` with `line` through PATH.tmp and a rename, so a reader sees
/// either the previous line or `line`. No flush to disk: the file is rewritten every
/// few seconds and is not needed after a crash.
bool write_line(const std::filesystem::path& path, const std::string& line);

/// Writes snapshot() to a file when constructed, then every `period` from its
/// own thread, and once more when destroyed (after the thread has stopped). A
/// write that fails (on Windows, a reader holding the file open) is retried at
/// the next tick.
class FileWriter {
public:
    FileWriter(std::filesystem::path path, std::chrono::milliseconds period);
    ~FileWriter();
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

private:
    void loop();

    std::filesystem::path path_;
    std::chrono::milliseconds period_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread th_;
};

} // namespace collatz::progress

#endif // COLLATZ_PROGRESS_HPP
