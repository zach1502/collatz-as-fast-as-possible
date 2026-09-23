#ifndef COLLATZ_WORKER_CONFIG_HPP
#define COLLATZ_WORKER_CONFIG_HPP

// The config file of `collatz worker` (collatz/worker.hpp) with the host id,
// the owner-only directories it is written to, the worker's state directory,
// the OS random source, and the worker token file.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace collatz::worker {

/// Identity and defaults, stored as JSON at config_path(). One host id per
/// config file.
struct Config {
    std::string host_id; ///< 32 lowercase hex, random
    std::string server;  ///< last --server
};

/// %APPDATA%\collatz\worker.json (8.3 short form when the path is not
/// ASCII); empty when APPDATA is unset or has no ASCII form.
[[nodiscard]] std::filesystem::path default_config_path();

/// A missing file yields an empty Config and true; a file that does not parse
/// (or has malformed fields) yields false with a reason.
[[nodiscard]] bool load_config(const std::filesystem::path& p, Config& out, std::string& err);

/// Creates the missing directories of `dir` owner-only (a DACL for the owner
/// and SYSTEM that new files inherit); directories that already exist keep
/// their permissions.
[[nodiscard]] bool make_private_dirs(const std::filesystem::path& dir, std::string& err);

/// The directory that holds everything `collatz worker` deletes or changes
/// (its request files, units, audits, lock and PAUSE file): `collatz-worker`
/// next to the config file `config_path`, so that a config file placed among
/// other files leaves them alone.
[[nodiscard]] std::filesystem::path state_dir(const std::filesystem::path& config_path);

/// The marker file open_state_dir writes into a state directory it creates.
inline constexpr const char* kStateMarker = ".collatz-worker";

/// Creates `dir` owner-only (make_private_dirs) with the marker file
/// kStateMarker, or checks that an existing `dir` is one the worker made: a
/// directory, not a symlink, holding the marker; an empty directory gets the
/// marker. False with a reason otherwise, so that the worker never deletes or
/// changes files in a directory it did not make.
[[nodiscard]] bool open_state_dir(const std::filesystem::path& dir, std::string& err);

/// Atomic write (a new temp file in the same directory, then a rename). The
/// file is created owner-only (a DACL for the owner and SYSTEM) before
/// anything is written to it. Missing directories are created owner-only the
/// same way; directories that exist are left as they are.
[[nodiscard]] bool save_config(const std::filesystem::path& p, const Config& c, std::string& err);

/// `n` bytes from the OS random source (BCryptGenRandom). False when the
/// source fails.
[[nodiscard]] bool random_bytes(unsigned char* out, std::size_t n) noexcept;

/// random_bytes(`bytes`) as lowercase hex; empty when the OS source fails.
[[nodiscard]] std::string random_hex(std::size_t bytes);

/// The server's worker token (its WORKER_TOKEN): 32 to 1024 bytes of
/// printable ASCII without spaces.
[[nodiscard]] bool valid_worker_token(std::string_view t) noexcept;

/// Reads the worker token from the file `p`: its text without a leading UTF-8
/// byte order mark and surrounding spaces, tabs, CR and LF, so a file saved by
/// a Windows editor works. False with a reason that does not quote the file's
/// text when it cannot be read or holds no valid_worker_token.
[[nodiscard]] bool
read_worker_token(const std::filesystem::path& p, std::string& token, std::string& err);

} // namespace collatz::worker

#endif // COLLATZ_WORKER_CONFIG_HPP
