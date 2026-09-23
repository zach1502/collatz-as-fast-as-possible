#ifndef COLLATZ_CORE_WORKER_INTERNAL_HPP
#define COLLATZ_CORE_WORKER_INTERNAL_HPP

// Private helpers shared by the src/core/worker*.cpp pieces of the worker
// client, grouped by the file that defines them.

#include <filesystem>
#include <string>
#include <string_view>

namespace collatz::worker::detail {

// Defined in worker.cpp.
std::string ascii_path(const std::wstring& w);
std::wstring widen(const std::string& s);
std::wstring find_program(const std::wstring& name);
bool create_private_file(const std::filesystem::path& p,
                         const std::string& content,
                         bool sync,
                         std::string& err);

// Defined in worker_config.cpp.
bool read_text(const std::filesystem::path& p, std::string& out);

// Defined in worker_digest.cpp.
bool is_decimal(std::string_view s);

// Defined in worker_lock.cpp.
long long current_pid();

} // namespace collatz::worker::detail

#endif // COLLATZ_CORE_WORKER_INTERNAL_HPP
