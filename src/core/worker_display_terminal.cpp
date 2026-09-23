// Terminal output and checks of `collatz worker`: write_terminal,
// local_time_text, the VT mode and UTF-8 code page of a Windows console, the
// MSYS2 pty test, the locale test and the window size.

// Windows: GetFileInformationByHandleEx (FileNameInfo) needs _WIN32_WINNT >=
// 0x0600. Set before the first header, since <_mingw.h> picks a default.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "collatz/worker_display.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004 // some MinGW headers lack it
#endif

namespace collatz::worker {

void write_terminal(std::string_view s) {
    std::fflush(stderr);
    std::fflush(stdout);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        bool ok = true;
        while (ok && !s.empty()) {
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(s.size(), 1u << 20));
            ok = WriteFile(h, s.data(), chunk, &written, nullptr) != 0 && written > 0;
            s.remove_prefix(ok ? written : 0);
        }
        if (s.empty()) {
            return;
        }
    }
    (void) std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

std::string local_time_text(std::time_t t, bool with_date) {
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) {
        return "?";
    }
    char b[32];
    std::size_t n = 0;
    if (with_date) {
        n = std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        n = std::strftime(b, sizeof(b), "%H:%M", &tm);
    }
    return n > 0 ? std::string(b, n) : std::string("?");
}

// NOLINTNEXTLINE(bugprone-exception-escape): every substr starts at most at size()
bool is_msys_pty_name(std::wstring_view name) noexcept {
    const std::size_t slash = name.find_last_of(L'\\');
    const std::wstring_view base = slash == std::wstring_view::npos ? name : name.substr(slash + 1);
    const bool msys = base.substr(0, 5) == L"msys-" || base.substr(0, 7) == L"cygwin-";
    return msys && base.find(L"-pty") != std::wstring_view::npos;
}

bool locale_is_utf8(const char* lc_all, const char* lc_ctype, const char* lang) noexcept {
    const char* v = nullptr;
    for (const char* c : {lc_all, lc_ctype, lang}) {
        if (c != nullptr && c[0] != '\0') {
            v = c;
            break;
        }
    }
    if (v == nullptr) {
        return false;
    }
    std::string lower;
    for (const char* p = v; *p != '\0' && lower.size() < 64; ++p) {
        lower += (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
    }
    return lower.find("utf-8") != std::string::npos || lower.find("utf8") != std::string::npos;
}

namespace {

bool env_utf8() {
    return locale_is_utf8(std::getenv("LC_ALL"), std::getenv("LC_CTYPE"), std::getenv("LANG"));
}

DWORD g_saved_mode = 0;
bool g_mode_changed = false;
UINT g_saved_cp = 0;
bool g_cp_changed = false;

/// stdout is a pipe of an MSYS2/Cygwin pty (mintty), which is no console.
bool msys_pty(HANDLE h) {
    if (GetFileType(h) != FILE_TYPE_PIPE) {
        return false;
    }
    // FILE_NAME_INFO is a length and a name that runs past its one-element
    // array, so it is read from a byte buffer.
    alignas(
        FILE_NAME_INFO) unsigned char buf[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(WCHAR)] = {};
    if (GetFileInformationByHandleEx(h, FileNameInfo, buf, static_cast<DWORD>(sizeof(buf))) == 0) {
        return false;
    }
    DWORD bytes = 0;
    std::memcpy(&bytes, buf + offsetof(FILE_NAME_INFO, FileNameLength), sizeof(bytes));
    const std::size_t room = sizeof(buf) - offsetof(FILE_NAME_INFO, FileName);
    const std::size_t n = std::min<std::size_t>(bytes, room) / sizeof(WCHAR);
    std::wstring name(n, L'\0');
    std::memcpy(name.data(), buf + offsetof(FILE_NAME_INFO, FileName), n * sizeof(WCHAR));
    return is_msys_pty_name(name);
}

} // namespace

Terminal open_terminal(bool ascii) {
    Terminal t;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return t;
    }
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode) != 0) {
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
            // Fails on consoles before Windows 10: plain lines there.
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
                return t;
            }
            g_saved_mode = mode;
            g_mode_changed = true;
        }
        t.ansi = true;
        if (!ascii) {
            const UINT cp = GetConsoleOutputCP();
            if (cp == CP_UTF8) {
                t.utf8 = true;
            } else if (SetConsoleOutputCP(CP_UTF8) != 0) {
                g_saved_cp = cp;
                g_cp_changed = true;
                t.utf8 = true;
            }
        }
        return t;
    }
    if (msys_pty(h)) {
        t.ansi = true;
        t.msys_pty = true;
        t.utf8 = !ascii && env_utf8();
    }
    return t;
}

void restore_terminal() noexcept {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_mode_changed && h != nullptr && h != INVALID_HANDLE_VALUE) {
        (void) SetConsoleMode(h, g_saved_mode);
        g_mode_changed = false;
    }
    if (g_cp_changed) {
        (void) SetConsoleOutputCP(g_saved_cp);
        g_cp_changed = false;
    }
}

unsigned terminal_width() noexcept {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0) {
        const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (w > 0) {
            return static_cast<unsigned>(w);
        }
    }
    if (const char* c = std::getenv("COLUMNS"); c != nullptr) {
        const long v = std::strtol(c, nullptr, 10);
        if (v >= 20 && v <= 1000) {
            return static_cast<unsigned>(v);
        }
    }
    return 80;
}

unsigned terminal_height() noexcept {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0) {
        const int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (h > 0) {
            return static_cast<unsigned>(h);
        }
    }
    return 0;
}

} // namespace collatz::worker
