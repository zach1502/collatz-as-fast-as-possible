#ifndef COLLATZ_WORKER_PROCESS_HPP
#define COLLATZ_WORKER_PROCESS_HPP

// Child processes of `collatz worker` (run_process, their exit codes as text,
// whether a program can be started), process ids, the path of this executable
// and the lock file.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace collatz::worker {

/// One argument quoted for a Windows command line, so that CommandLineToArgvW
/// and the MinGW/MSVCRT startup code give back `a` unchanged.
[[nodiscard]] std::string quote_windows_arg(std::string_view a);

struct ProcessOptions {
    std::string stdout_path; ///< file for the child's stdout; empty = inherit
    std::string stderr_path; ///< file for stderr; may equal stdout_path; empty = inherit
    /// Called about every 200 ms while the child runs.
    std::function<void()> poll;
    /// Called once with the child's process id right after it started.
    std::function<void(long long)> started;
    /// The child runs at the lowest CPU priority (IDLE_PRIORITY_CLASS).
    bool low_priority = false;
    /// Polled about every 200 ms while the child runs; once it returns true the
    /// child is terminated (TerminateProcess with exit code kTerminatedExit) and
    /// run_process returns when it has exited. The process handle stays open
    /// until then, so the call cannot reach another process.
    std::function<bool()> cancel;
};

/// run_process's return value for a child that `cancel` terminated: the exit
/// code it passes to TerminateProcess.
inline constexpr int kTerminatedExit = 137;

/// Runs argv without a shell and waits. A program name without a directory is
/// searched in the system directory, then in the absolute PATH entries, and
/// never in the current directory. Returns the exit code; when the child could
/// not start (or could not be waited for) it returns -1 with `err` set. Only
/// that case sets `err`: a Windows exit code is 32 bits and comes back as its
/// int bit pattern, so -1 and other negative values are exit codes too.
[[nodiscard]] int
run_process(const std::vector<std::string>& argv, const ProcessOptions& o, std::string& err);

/// An exit code of run_process as text: decimal up to 65535, else 8 hex
/// digits ("0xC0000005"), the form of a Windows NTSTATUS such as an access
/// violation.
[[nodiscard]] std::string exit_code_text(int code);

/// Whether run_process could start `program`: an executable regular file at
/// that path when it names a directory, else one found where run_process
/// searches (the system directory, then the absolute PATH entries). Empty
/// names nothing.
[[nodiscard]] bool program_found(const std::string& program);

/// True while a process with this id exists (OpenProcess); an id the OS has
/// reused for another process also counts.
[[nodiscard]] bool process_alive(long long pid) noexcept;

/// Path of this executable, used to re-invoke `collatz verify`; "collatz"
/// (searched on PATH) when the OS does not say.
[[nodiscard]] std::string self_executable();

/// Takes the exclusive lock on the file `p` (created when missing) with
/// LockFileEx, held until release_lock or the end of the process, however it
/// ends. The file holds this process's id, which only names the holder in a
/// message; a file left by a process that is gone is free whatever id it
/// holds. False with the holder's pid in `err` when another process (or
/// another call of this one) holds the lock. Not thread-safe; the worker calls
/// these from its main thread.
[[nodiscard]] bool acquire_lock(const std::filesystem::path& p, std::string& err);
/// Releases the lock acquire_lock took on `p`; the file stays.
void release_lock(const std::filesystem::path& p) noexcept;

} // namespace collatz::worker

#endif // COLLATZ_WORKER_PROCESS_HPP
