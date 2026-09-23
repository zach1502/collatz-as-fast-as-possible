// The read side of the Step-2 cache platform layer: read-only whole-file mappings
// (Win32 file mapping) with the read-ahead hint, and mmap_prefetch_enabled.

#include "collatz/step2_cache.hpp"
#include "step2_cache_internal.hpp"

#include <cstdint>
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

namespace {

// COLLATZ_MMAP_WILLNEED=0 turns off the mapping read-ahead hint
// (PrefetchVirtualMemory; default on).
// Read once per process.
bool mmap_willneed_on() {
    static const bool on = [] {
        const char* e = std::getenv("COLLATZ_MMAP_WILLNEED");
        return !(e != nullptr && e[0] == '0' && e[1] == '\0');
    }();
    return on;
}

} // namespace

// PrefetchVirtualMemory and WIN32_MEMORY_RANGE_ENTRY come from <windows.h>
// (memoryapi.h) when the SDK targets Windows 8 or later; MSYS2's mingw-w64
// targets Windows 10.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#error "the Step-2 cache needs _WIN32_WINNT >= 0x0602 (Windows 8) for PrefetchVirtualMemory"
#endif

namespace {

using PrefetchRange = WIN32_MEMORY_RANGE_ENTRY;
using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);

/// The read-ahead entry point, or nullptr when COLLATZ_MMAP_WILLNEED=0. Resolved
/// only once per process.
PrefetchFn prefetch_fn() noexcept {
    static const PrefetchFn fn = mmap_willneed_on() ? &::PrefetchVirtualMemory : nullptr;
    return fn;
}

} // namespace

// Maps an existing file read-only in its entirety. 64-bit throughout: the size
// comes from GetFileSizeEx (LARGE_INTEGER); CreateFileMappingW with a 0/0
// maximum size uses the file's real size; MapViewOfFile with a 0 byte count
// maps the whole file, so multi-GB artifacts (2 GB bv + 4 GB dip at B=31)
// never truncate through a 32-bit length. All handles share R/W/DELETE so a
// concurrent atomic replace of this file cannot hit a sharing violation.
Mapping map_readonly(const std::string& path) {
    Mapping m;
    const std::wstring wpath = to_wide(path);
    if (wpath.empty()) {
        return m;
    }
    HANDLE file = ::CreateFileW(wpath.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return m;
    }
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(file, &sz) == 0 || sz.QuadPart <= 0) {
        ::CloseHandle(file);
        return m;
    }
    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        ::CloseHandle(file);
        return m;
    }
    void* addr = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (addr == nullptr) {
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        return m;
    }
    m.addr = addr;
    m.size = static_cast<std::uint64_t>(sz.QuadPart);
    m.file = file;
    m.mapping = mapping;
    // The read-ahead hint: without it a cold multi-GB view makes the GPU bv
    // upload fault one page at a time.
    // PrefetchVirtualMemory issues large concurrent reads of the whole view into
    // the standby list. Advisory only: the bytes and the payload hash are
    // unchanged and a failure is ignored. No residency gate: a prefetch of an
    // already-resident file is cheap. COLLATZ_MMAP_WILLNEED=0 disables it.
    if (const PrefetchFn fn = prefetch_fn(); fn != nullptr) {
        PrefetchRange range{};
        range.VirtualAddress = addr;
        range.NumberOfBytes = static_cast<SIZE_T>(sz.QuadPart);
        (void) fn(::GetCurrentProcess(), 1, &range, 0);
    }
    return m;
}

void unmap(Mapping& m) noexcept {
    if (m.addr != nullptr) {
        ::UnmapViewOfFile(m.addr);
    }
    if (m.mapping != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m.mapping));
    }
    if (m.file != nullptr && m.file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(static_cast<HANDLE>(m.file));
    }
    m = Mapping{};
}

} // namespace collatz::step2::cache::detail::platform

namespace collatz::step2::cache {

using namespace detail;

bool mmap_prefetch_enabled() {
    return platform::prefetch_fn() != nullptr;
}

} // namespace collatz::step2::cache
