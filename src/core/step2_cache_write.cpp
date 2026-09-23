// The write side of the Step-2 cache platform layer: the atomic temp-file write
// and MoveFileExW, the UTF-16 paths of the Win32 calls, the best-effort
// directory-tree creation, and the cache directory.
//
// write_atomic_stream writes the header, the payload, then the header again at
// offset 0, so a producer that hashes the payload while streaming it can store
// the hash in the header. The rename happens only after all three succeed.

#include "collatz/step2_cache.hpp"
#include "step2_cache_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // keep std::min/std::max, not the windows.h macros
#endif
#include <windows.h>

namespace collatz::step2::cache::detail::platform {

// Converts a UTF-8 std::string to UTF-16 for the wide (...W) Win32 APIs, so paths
// containing non-ASCII characters (e.g. a profile name) work regardless of the
// ANSI code page. Returns empty on failure; callers treat that as a miss.
std::wstring to_wide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int need =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    const int got =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), need);
    if (got <= 0) {
        return std::wstring();
    }
    w.resize(static_cast<std::size_t>(got));
    return w;
}

// Writes header+payload to a uniquely-named temp file in the same directory as
// final_path, flushes it, then moves it into place with MoveFileExW(...,
// MOVEFILE_REPLACE_EXISTING). Best effort: any failure deletes the temp and
// returns false. FILE_SHARE_DELETE on all handles (here and in map_readonly)
// lets the replace succeed even if another process has the destination mapped;
// if the replace still fails, the temp is deleted and the store reports false.
bool write_atomic_stream(const std::string& final_path,
                         const void* header,
                         std::size_t header_len,
                         const PayloadProducer& produce) {
    const std::wstring wfinal = to_wide(final_path);
    if (wfinal.empty()) {
        return false;
    }
    const std::string tmp =
        final_path + ".tmp." + std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
    const std::wstring wtmp = to_wide(tmp);
    if (wtmp.empty()) {
        return false;
    }
    HANDLE fh = ::CreateFileW(wtmp.c_str(),
                              GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }
    // WriteFile takes a 32-bit DWORD byte count, so a multi-GB payload must be
    // written in chunks; cap at 1 GiB.
    auto write_all = [fh](const void* buf, std::uint64_t n) -> bool {
        const auto* p = static_cast<const unsigned char*>(buf);
        constexpr std::uint64_t kChunk = std::uint64_t{1} << 30;
        std::uint64_t done = 0;
        while (done < n) {
            const std::uint64_t want = std::min(kChunk, n - done);
            DWORD wrote = 0;
            if (::WriteFile(fh, p + done, static_cast<DWORD>(want), &wrote, nullptr) == 0 ||
                wrote == 0) {
                return false;
            }
            done += wrote;
        }
        return true;
    };
    // The cache must never fault a verification: any exception from the
    // producer (bad_alloc, std::system_error from a helper thread) is a failed
    // store: close the handle, delete the temp, return false.
    bool ok = false;
    try {
        ok = write_all(header, header_len) && produce(write_all);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        const LARGE_INTEGER start{};
        ok = ::SetFilePointerEx(fh, start, nullptr, FILE_BEGIN) != 0 &&
             write_all(header, header_len);
    }
    if (ok) {
        ok = (::FlushFileBuffers(fh) != 0);
    }
    ::CloseHandle(fh);
    if (!ok || ::MoveFileExW(wtmp.c_str(), wfinal.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        ::DeleteFileW(wtmp.c_str());
        return false;
    }
    return true;
}

namespace {

// Best-effort `mkdir -p`: creates each path prefix, ignoring "already exists"
// and any other error (the caller falls back to a disabled cache). Accepts both
// '\\' and '/' separators; skips a bare drive spec like "C:".
void make_dirs(const std::string& dir) {
    const std::wstring w = to_wide(dir);
    std::wstring acc;
    for (std::size_t i = 0; i <= w.size(); ++i) {
        if (i == w.size() || w[i] == L'\\' || w[i] == L'/') {
            const bool bare_drive = (acc.size() == 2 && acc[1] == L':');
            if (!acc.empty() && !bare_drive) {
                ::CreateDirectoryW(acc.c_str(), nullptr); // ignore ERROR_ALREADY_EXISTS
            }
        }
        if (i < w.size()) {
            acc.push_back(w[i]);
        }
    }
}

} // namespace

// Contiguous-buffer form of write_atomic_stream.
bool write_atomic(const std::string& final_path,
                  const void* header,
                  std::size_t header_len,
                  const void* payload,
                  std::uint64_t payload_bytes) {
    return write_atomic_stream(
        final_path, header, header_len, [payload, payload_bytes](const PayloadSink& sink) {
            return sink(payload, payload_bytes);
        });
}

} // namespace collatz::step2::cache::detail::platform

namespace collatz::step2::cache {

using namespace detail;

std::string resolve_dir() {
    // $COLLATZ_CACHE_DIR first, else %LOCALAPPDATA%\collatz, else "" (cache
    // disabled). Env values are treated as UTF-8 and converted to UTF-16 for
    // the W APIs inside the platform layer (UCRT delivers UTF-8).
    std::string dir;
    if (const char* env = std::getenv("COLLATZ_CACHE_DIR"); env != nullptr && env[0] != '\0') {
        dir = env;
    } else if (const char* local = std::getenv("LOCALAPPDATA");
               local != nullptr && local[0] != '\0') {
        dir = std::string(local) + "\\collatz";
    } else {
        return {};
    }
    platform::make_dirs(dir);
    sweep_stale_temps(dir);
    return dir;
}

} // namespace collatz::step2::cache
