#pragma once

// Per-process temp cache dirs for the Step-2 cache unit tests
// (test_step2_cache*.cpp), removed when the process exits, and a byte patch of a
// cache file's payload.
#include "collatz/step2_cache.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// The CRT's _getpid(); the pid only makes the per-run temp cache dir unique.
#include <process.h>

namespace collatz::test {

inline long test_pid() {
    return static_cast<long>(_getpid());
}

// The temp dirs this process made, removed when it exits (after main returns),
// so test runs leave nothing in the temp root. A process killed by a timeout
// still leaves its dirs.
class TempDirsAtExit {
public:
    TempDirsAtExit() = default;
    TempDirsAtExit(const TempDirsAtExit&) = delete;
    TempDirsAtExit& operator=(const TempDirsAtExit&) = delete;
    ~TempDirsAtExit() {
        for (const std::filesystem::path& p : dirs_) {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    }
    void add(const std::filesystem::path& p) {
        dirs_.push_back(p);
    }

private:
    std::vector<std::filesystem::path> dirs_;
};

inline void remove_at_exit(const std::filesystem::path& p) {
    static TempDirsAtExit dirs;
    dirs.add(p);
}

inline std::string unique_tmp_dir() {
    // std::filesystem::temp_directory_path() is the temp root (%TMP% or %TEMP%).
    const std::filesystem::path base = std::filesystem::temp_directory_path() /
                                       ("collatz_cache_test_" + std::to_string(test_pid()));
    const std::string dir = base.string();
    collatz::test::set_env("COLLATZ_CACHE_DIR", dir.c_str());
    remove_at_exit(base);
    return collatz::step2::cache::resolve_dir(); // creates the directory chain
}

// An empty cache dir of its own for one test case, set as COLLATZ_CACHE_DIR.
inline std::string fresh_cache_dir(const std::string& tag) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                    ("collatz_cache_" + tag + "_" + std::to_string(test_pid()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    remove_at_exit(p);
    const std::string dir = p.string();
    collatz::test::set_env("COLLATZ_CACHE_DIR", dir.c_str());
    return dir;
}

// Replaces bytes [off, off + bytes.size()) of the payload of cache file `path`
// with `bytes` and returns the bytes it replaced. The payload is the last
// `payload` bytes of the file, so the patch does not depend on the header size.
inline std::vector<unsigned char> patch_payload(const std::string& path,
                                                std::uint64_t payload,
                                                std::uint64_t off,
                                                const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    std::vector<unsigned char> old(bytes.size());
    if (ec || size < payload || off + bytes.size() > payload) {
        return {};
    }
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    const auto at = static_cast<std::streamoff>(size - payload + off);
    f.seekg(at);
    f.read(reinterpret_cast<char*>(old.data()), static_cast<std::streamsize>(old.size()));
    f.seekp(at);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good() ? old : std::vector<unsigned char>{};
}

// The 8-byte word at payload offset `off` of cache file `path` (0 if unreadable).
inline std::uint64_t
payload_word(const std::string& path, std::uint64_t payload, std::uint64_t off) {
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    std::uint64_t w = 0;
    if (ec || size < payload || off + sizeof(w) > payload) {
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(size - payload + off));
    f.read(reinterpret_cast<char*>(&w), sizeof(w));
    return f.good() ? w : 0;
}

} // namespace collatz::test
