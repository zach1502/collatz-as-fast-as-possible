// Removes the temp files that a cache write leaves when its process dies
// before the rename (write_atomic_stream names them <cache file>.tmp.<pid>; one
// is up to 8 GiB at B=32). resolve_dir runs it on every cache directory it
// returns.

#include "collatz/step2_cache.hpp"
#include "step2_cache_internal.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // keep std::min/std::max, not the windows.h macros
#endif
#include <windows.h>

namespace collatz::step2::cache::detail {

namespace {

std::uint64_t current_pid() noexcept {
    return ::GetCurrentProcessId();
}

// False only when Windows reports that no process has this id; a process this
// user may not open counts as alive.
bool process_alive(std::uint64_t pid) noexcept {
    if (pid == 0 || pid > 0xFFFFFFFFull) {
        return true;
    }
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(h, &code) == 0 || code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
}

bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

bool ends_with(const std::string& s, const char* p) {
    const std::string tail(p);
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// True when `name` is "<dip or bv cache file name>.tmp.<pid>"; sets `pid`
// (at most 10 digits, so it cannot overflow).
bool temp_name(const std::string& name, std::uint64_t& pid) {
    const std::size_t at = name.rfind(".tmp.");
    if (at == std::string::npos || at + 5 >= name.size() || name.size() - at - 5 > 10) {
        return false;
    }
    pid = 0;
    for (std::size_t i = at + 5; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        pid = pid * 10u + static_cast<std::uint64_t>(name[i] - '0');
    }
    const std::string base = name.substr(0, at);
    return (starts_with(base, "dip_B") && ends_with(base, ".u16")) ||
           (starts_with(base, "bv_B") && ends_with(base, ".bin"));
}

// A temp file this young is left alone even when its pid looks dead: a writer
// whose pid this process cannot see (on another machine or in a container
// that shares the directory) still appends to it, which keeps its write time
// current.
constexpr std::chrono::minutes kMinAge{10};

void sweep(const std::string& dir) {
    namespace fs = std::filesystem;
    const fs::path root(platform::to_wide(dir));
    const auto now = fs::file_time_type::clock::now();
    const std::uint64_t self = current_pid();
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::u8string u8 = it->path().filename().u8string();
        std::string name;
        for (const char8_t c : u8) {
            name.push_back(static_cast<char>(c));
        }
        std::uint64_t pid = 0;
        std::error_code fe;
        if (!temp_name(name, pid) || pid == self || !it->is_regular_file(fe)) {
            continue;
        }
        const fs::file_time_type written = it->last_write_time(fe);
        if (fe || now - written < kMinAge || process_alive(pid)) {
            continue;
        }
        if (fs::remove(it->path(), fe) && !fe) {
            std::fprintf(stderr,
                         "step2 cache: removed %s, left by process %llu\n",
                         name.c_str(),
                         static_cast<unsigned long long>(pid));
        }
    }
}

} // namespace

void sweep_stale_temps(const std::string& dir) noexcept {
    // Best effort: an unreadable directory or entry leaves the files in place.
    try {
        sweep(dir);
    } catch (...) { // NOLINT(bugprone-empty-catch): the sweep never fails a run
    }
}

} // namespace collatz::step2::cache::detail
