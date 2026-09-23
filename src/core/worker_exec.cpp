// Whether a program can be started (program_found, checked before the worker
// relies on curl) and the text of a child's exit code (exit_code_text).

#include "collatz/worker.hpp"
#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

namespace collatz::worker {

namespace fs = std::filesystem;
using namespace detail;

std::string exit_code_text(int code) {
    if (code >= 0 && code <= 0xFFFF) {
        return std::to_string(code);
    }
    char buf[16];
    std::snprintf(
        buf, sizeof(buf), "0x%08X", static_cast<unsigned>(static_cast<std::uint32_t>(code)));
    return buf;
}

bool program_found(const std::string& program) {
    if (program.empty()) {
        return false;
    }
    const std::wstring app = find_program(widen(program));
    std::error_code ec;
    return !app.empty() && fs::is_regular_file(fs::path(app), ec);
}

} // namespace collatz::worker
