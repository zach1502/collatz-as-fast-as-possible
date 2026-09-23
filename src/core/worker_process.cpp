// Child processes of the worker client: Windows command-line quoting,
// run_process (CreateProcessW) and self_executable.

#include "collatz/worker.hpp"
#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

namespace collatz::worker {

namespace fs = std::filesystem;
using namespace detail;

std::string quote_windows_arg(std::string_view a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(a);
    }
    // Backslashes are literal unless they precede a quote; double those, and
    // the run before the closing quote.
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : a) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
        } else {
            out.append(slashes, '\\');
        }
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    out += '"';
    return out;
}

int run_process(const std::vector<std::string>& argv, const ProcessOptions& o, std::string& err) {
    if (argv.empty()) {
        err = "empty command";
        return -1;
    }
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            cmd += L' ';
        }
        cmd += widen(quote_windows_arg(argv[i]));
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE hin = INVALID_HANDLE_VALUE;
    HANDLE hout = INVALID_HANDLE_VALUE;
    HANDLE herr = INVALID_HANDLE_VALUE;
    const auto open_out = [&sa](const std::string& path) {
        return CreateFileW(widen(path).c_str(),
                           GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    };
    if (!o.stdout_path.empty() || !o.stderr_path.empty()) {
        si.dwFlags = STARTF_USESTDHANDLES;
        hin = CreateFileW(L"NUL",
                          GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          &sa,
                          OPEN_EXISTING,
                          0,
                          nullptr);
        hout = o.stdout_path.empty() ? INVALID_HANDLE_VALUE : open_out(o.stdout_path);
        if (o.stderr_path == o.stdout_path) {
            herr = hout;
        } else if (!o.stderr_path.empty()) {
            herr = open_out(o.stderr_path);
        }
        si.hStdInput = hin;
        si.hStdOutput = hout != INVALID_HANDLE_VALUE ? hout : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = herr != INVALID_HANDLE_VALUE ? herr : GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    const std::wstring app = find_program(widen(argv[0]));
    const bool ok =
        !app.empty() &&
        CreateProcessW(app.c_str(),
                       cmd.data(),
                       nullptr,
                       nullptr,
                       TRUE,
                       o.low_priority ? static_cast<DWORD>(IDLE_PRIORITY_CLASS) : DWORD{0},
                       nullptr,
                       nullptr,
                       &si,
                       &pi) != 0;
    const DWORD start_error = ok ? 0 : GetLastError();
    for (HANDLE h : {hin, hout, herr == hout ? INVALID_HANDLE_VALUE : herr}) {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
    if (app.empty()) {
        err = "cannot find " + argv[0] + " in the system directory or on PATH";
        return -1;
    }
    if (!ok) {
        err = "cannot start " + argv[0] + " (Windows error " + std::to_string(start_error) + ")";
        return -1;
    }
    CloseHandle(pi.hThread);
    if (o.started) {
        o.started(static_cast<long long>(pi.dwProcessId));
    }
    bool terminated = false;
    for (;;) {
        const DWORD w = WaitForSingleObject(pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (w == WAIT_FAILED) {
            CloseHandle(pi.hProcess);
            err = "wait failed for " + argv[0];
            return -1;
        }
        if (o.poll) {
            o.poll();
        }
        if (!terminated && o.cancel && o.cancel()) {
            // The handle keeps the process object, so this cannot hit another process.
            terminated = TerminateProcess(pi.hProcess, static_cast<UINT>(kTerminatedExit)) != 0;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    // The DWORD's bit pattern (the C++20 conversion is modular): an NTSTATUS
    // such as 0xC0000005 comes back negative, and err stays empty.
    return static_cast<int>(code);
}

std::string self_executable() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) {
        buf.resize(n);
        const std::string p = ascii_path(buf);
        if (!p.empty()) {
            return p;
        }
    }
    return "collatz";
}

} // namespace collatz::worker
