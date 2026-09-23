// The process-wide progress counters and their snapshot. progress_file.cpp
// formats, parses and writes the progress file.

#include "collatz/progress.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace collatz::progress {

namespace {

// Slice transitions, task starts and snapshots take `m`, so a snapshot never
// pairs a finished slice's task counts with the old slice count (a backwards
// jump of one slice). The per-task and per-batch counts are lock-free adds:
// every task and survivor of a slice is finished before that slice's
// transition.
struct Counters {
    std::mutex m;
    std::atomic<std::uint64_t> tasks_done{0};
    std::atomic<std::uint64_t> survivors_found{0};
    std::atomic<std::uint64_t> survivors_done{0};
    // Per task class: the finished tasks, the survivors they found, all tasks.
    std::array<std::atomic<std::uint64_t>, kTaskClasses> class_done{};
    std::array<std::atomic<std::uint64_t>, kTaskClasses> class_found{};
    std::array<std::uint64_t, kTaskClasses> class_total{};
    std::uint64_t tasks_total = 0;
    std::uint64_t slices_done = 0;
    std::uint64_t slices_total = 0; ///< 0 = no checkpoint layer
    std::uint64_t slices_resumed = 0;
};

// A function-local static: a std::mutex member is not constant-initialized on
// every standard library, and this is first used from a thread of
// step1/checkpoint, never during static initialization.
Counters& counters() noexcept {
    static Counters c;
    return c;
}

// Caller holds c.m.
void zero_tasks(Counters& c) noexcept {
    c.tasks_done.store(0);
    c.survivors_found.store(0);
    c.survivors_done.store(0);
    for (unsigned i = 0; i < kTaskClasses; ++i) {
        c.class_done[i].store(0);
        c.class_found[i].store(0);
        c.class_total[i] = 0;
    }
    c.tasks_total = 0;
}

} // namespace

void start_tasks(std::span<const std::uint64_t> per_class) noexcept {
    Counters& c = counters();
    const std::lock_guard<std::mutex> lk(c.m);
    zero_tasks(c);
    for (std::size_t i = 0; i < per_class.size(); ++i) {
        c.class_total[i < kTaskClasses ? i : kTaskClasses - 1] += per_class[i];
        c.tasks_total += per_class[i];
    }
}

void task_finished(unsigned cls, std::uint64_t survivors) noexcept {
    Counters& c = counters();
    const unsigned i = task_class(cls);
    c.class_found[i].fetch_add(survivors, std::memory_order_relaxed);
    // Release: a snapshot that sees this task finished also sees its survivors.
    c.class_done[i].fetch_add(1, std::memory_order_release);
    c.tasks_done.fetch_add(1, std::memory_order_relaxed);
}

void survivors_found(std::uint64_t n) noexcept {
    counters().survivors_found.fetch_add(n, std::memory_order_relaxed);
}

void survivors_done(std::uint64_t n) noexcept {
    counters().survivors_done.fetch_add(n, std::memory_order_relaxed);
}

SurvivorCounts survivor_counts() noexcept {
    const Counters& c = counters();
    return {c.survivors_found.load(), c.survivors_done.load()};
}

void set_slices(std::uint64_t done, std::uint64_t total, std::uint64_t resumed) noexcept {
    Counters& c = counters();
    const std::lock_guard<std::mutex> lk(c.m);
    c.slices_done = done;
    c.slices_total = total;
    c.slices_resumed = resumed;
    zero_tasks(c);
}

Snapshot snapshot() noexcept {
    Counters& c = counters();
    Snapshot s;
    std::array<std::uint64_t, kTaskClasses> total{};
    std::array<std::uint64_t, kTaskClasses> done{};
    std::array<std::uint64_t, kTaskClasses> found{};
    std::uint64_t surv_found = 0;
    std::uint64_t surv_done = 0;
    {
        const std::lock_guard<std::mutex> lk(c.m);
        s.slices_done = c.slices_done;
        s.slices_total = c.slices_total;
        s.slices_resumed = c.slices_resumed;
        s.tasks_total = c.tasks_total;
        s.tasks_done = c.tasks_done.load();
        // The finished counts are read first (acquire, paired with
        // task_finished), then the survivors: a task that finishes in between
        // adds survivors but no finished task, which errs towards a larger total
        // and a smaller fraction.
        for (unsigned i = 0; i < kTaskClasses; ++i) {
            total[i] = c.class_total[i];
            done[i] = c.class_done[i].load(std::memory_order_acquire);
        }
        surv_done = c.survivors_done.load();
        surv_found = c.survivors_found.load();
        for (unsigned i = 0; i < kTaskClasses; ++i) {
            found[i] = c.class_found[i].load();
        }
    }
    if (s.slices_total == 0) {
        s.slices_total = 1;
        s.slices_done = 0;
        s.slices_resumed = 0;
    }
    s.slices_done = std::min(s.slices_done, s.slices_total);
    s.slices_resumed = std::min(s.slices_resumed, s.slices_done);
    s.tasks_done = std::min(s.tasks_done, s.tasks_total);

    std::uint64_t finished = 0;
    std::uint64_t finished_found = 0;
    for (unsigned i = 0; i < kTaskClasses; ++i) {
        done[i] = std::min(done[i], total[i]);
        finished += done[i];
        finished_found += found[i];
    }
    if (finished == 0) {
        return s; // "0 0": nothing to estimate from yet
    }
    std::uint64_t all = finished_found; // exact once every task has finished
    if (finished < s.tasks_total) {
        const double tasks = static_cast<double>(finished);
        const double per_task = static_cast<double>(finished_found) / tasks;
        double est = 0.0;
        for (unsigned i = 0; i < kTaskClasses; ++i) {
            if (total[i] == 0) {
                continue;
            }
            const double avg = done[i] > 0
                                   ? static_cast<double>(found[i]) / static_cast<double>(done[i])
                                   : per_task;
            est += static_cast<double>(total[i]) * avg;
        }
        // The found survivors scaled by the task fraction. It counts the
        // tasks still running as if they had finished, so it is the larger of
        // the two while few tasks are done (when the first ones found nothing,
        // the class averages say 0).
        const double ratio =
            static_cast<double>(surv_found) * static_cast<double>(s.tasks_total) / tasks;
        // 9.2e18 < 2^63 caps the conversion; no slice comes near it.
        all = static_cast<std::uint64_t>(std::min(std::ceil(std::max(est, ratio)), 9.2e18));
    }
    s.survivors_total = std::max({all, finished_found, surv_found});
    s.survivors_done = std::min(surv_done, s.survivors_total);
    return s;
}

double slice_fraction(const Snapshot& s) noexcept {
    if (s.tasks_total == 0) {
        return 0.0;
    }
    if (s.survivors_total > 0) {
        return static_cast<double>(s.survivors_done) / static_cast<double>(s.survivors_total);
    }
    return s.tasks_done >= s.tasks_total ? 1.0 : 0.0;
}

} // namespace collatz::progress
