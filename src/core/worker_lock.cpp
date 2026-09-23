// The process id and process liveness, the lock file (an OS lock that the
// worker holds while it runs; the file names the holder's pid), and the
// removal of audit directories an earlier session left.

#include "collatz/worker.hpp"
#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace collatz::worker {

namespace fs = std::filesystem;
using checkpoint::Json;
using namespace detail;

namespace detail {

long long current_pid() {
    return static_cast<long long>(GetCurrentProcessId());
}

} // namespace detail

bool process_alive(long long pid) noexcept {
    if (pid <= 0) {
        return false;
    }
    if (pid > 0xFFFFFFFFll) {
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok != 0 && code == STILL_ACTIVE;
}

std::size_t remove_stale_audit_dirs(const fs::path& audits_dir) {
    std::error_code ec;
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(audits_dir, ec)) {
        std::error_code e2;
        // Only the unit-id directories the worker made are its to remove.
        if (e.is_symlink(e2) || !e.is_directory(e2) || !is_decimal(e.path().filename().string())) {
            continue;
        }
        if (fs::exists(e.path() / kUnreportedAudit, e2)) {
            continue; // an outcome the server has not taken yet
        }
        std::ifstream in(e.path() / "audit.json", std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const std::optional<Json> doc = in ? checkpoint::parse_json(text.str()) : std::nullopt;
        const Json* f = doc ? doc->get("failures_total") : nullptr;
        if (f != nullptr && f->type == Json::Type::Number && f->text != "0") {
            continue; // a failed audit is kept for inspection
        }
        stale.push_back(e.path());
    }
    std::size_t removed = 0;
    for (const fs::path& d : stale) {
        std::error_code e2;
        fs::remove_all(d, e2);
        if (!e2) {
            ++removed;
        }
    }
    return removed;
}

namespace {

using LockHandle = HANDLE;

// The lock files this process holds and the handle that holds each lock. The
// OS drops a lock when its process ends, however it ends, so a lock file left
// behind is free and the pid written in it is only a name.
std::vector<std::pair<fs::path, LockHandle>>& held_locks() {
    static std::vector<std::pair<fs::path, LockHandle>> v;
    return v;
}

std::string holder_text(const fs::path& p) {
    std::string text;
    const long long pid = read_text(p, text) ? std::strtoll(text.c_str(), nullptr, 10) : 0;
    return (pid > 0 ? "process " + std::to_string(pid) : std::string("another process")) +
           " holds " + p.string();
}

} // namespace

bool acquire_lock(const fs::path& p, std::string& err) {
    const std::string pid = std::to_string(current_pid()) + "\n";
    // No security attributes: the handle is not inherited, so no verify or
    // curl child holds the lock.
    HANDLE h = CreateFileW(p.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err =
            "cannot open " + p.string() + " (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    // One byte at offset 2^32, past the pid text, which a second worker reads
    // for its message (a Windows lock also blocks reads of its range).
    OVERLAPPED ov{};
    ov.OffsetHigh = 1;
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) == 0) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        err = e == ERROR_LOCK_VIOLATION
                  ? holder_text(p)
                  : "cannot lock " + p.string() + " (Windows error " + std::to_string(e) + ")";
        return false;
    }
    DWORD written = 0;
    const bool ok =
        SetFilePointer(h, 0, nullptr, FILE_BEGIN) == 0 && SetEndOfFile(h) != 0 &&
        WriteFile(h, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr) != 0 &&
        written == pid.size();
    if (!ok) {
        CloseHandle(h);
    }
    if (!ok) {
        err = "cannot write " + p.string();
        return false;
    }
    held_locks().emplace_back(p, h);
    return true;
}

void release_lock(const fs::path& p) noexcept {
    auto& held = held_locks();
    for (auto it = held.begin(); it != held.end(); ++it) {
        if (it->first == p) {
            CloseHandle(it->second);
            held.erase(it);
            return;
        }
    }
}

} // namespace collatz::worker
