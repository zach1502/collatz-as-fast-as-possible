#pragma once

// Progress-line helpers shared by the progress, worker display and
// `verify --progress-file` tests.
#include "collatz/progress.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace collatz::test {

inline std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// The snapshot a reader gets from the progress line with these seven fields.
inline progress::Snapshot snap(std::uint64_t td,
                               std::uint64_t tt,
                               std::uint64_t sd,
                               std::uint64_t st,
                               std::uint64_t sr,
                               std::uint64_t vd,
                               std::uint64_t vt) {
    progress::Snapshot s;
    s.tasks_done = td;
    s.tasks_total = tt;
    s.slices_done = sd;
    s.slices_total = st;
    s.slices_resumed = sr;
    s.survivors_done = vd;
    s.survivors_total = vt;
    progress::Snapshot back;
    REQUIRE(progress::parse_line(progress::format_line(s), back));
    return back;
}

/// The progress line for the current snapshot, as the worker reads it back.
inline progress::Snapshot written() {
    progress::Snapshot s;
    REQUIRE(progress::parse_line(progress::format_line(progress::snapshot()), s));
    return s;
}

} // namespace collatz::test
