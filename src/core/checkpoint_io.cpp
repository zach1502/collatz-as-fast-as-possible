#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <io.h> // _commit, _fileno
#include <string>
#include <system_error>

// See collatz/checkpoint.hpp for the design. Everything here is C++20 and
// std::filesystem except _commit, the CRT call that flushes a file to disk.

namespace collatz::checkpoint {

namespace fs = std::filesystem;

using namespace detail;

namespace detail {

std::string host_name() {
    char host[256] = {0};
    return host;
}

bool readable_file(const fs::path& path, std::uintmax_t max_bytes, std::string& err) {
    std::error_code ec;
    // fs::status follows symlinks, so a link to a device is a device.
    const fs::file_status st = fs::status(path, ec);
    if (ec || !fs::exists(st)) {
        err = "cannot read file" + (ec ? " (" + ec.message() + ")" : std::string());
        return false;
    }
    if (!fs::is_regular_file(st)) {
        err = "not a regular file (a directory, device, FIFO or socket, or a link to one)";
        return false;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        err = "cannot read the file size (" + ec.message() + ")";
        return false;
    }
    if (size > max_bytes) {
        err = "file is " + std::to_string(size) + " bytes, above the " + std::to_string(max_bytes) +
              "-byte limit for a file this program writes";
        return false;
    }
    return true;
}

bool read_file(const std::string& path,
               std::string& out,
               std::string& err,
               std::uintmax_t max_bytes) {
    out.clear();
    if (!readable_file(fs::path(path), max_bytes, err)) {
        return false;
    }
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        err = "cannot open file";
        return false;
    }
    // Bounded by max_bytes also when the file grows after the size check.
    std::string buf(std::size_t{1} << 16, '\0');
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
        if (out.size() > max_bytes) {
            out.clear();
            err = "file grew above the " + std::to_string(max_bytes) + "-byte limit while read";
            return false;
        }
    }
    if (in.bad()) {
        err = "read error";
        return false;
    }
    return true;
}

// Write `content` to `path` atomically: temp file in the same directory,
// flush + _commit, then rename over the target. A crash leaves either the old
// file, the new file, or a stray *.tmp (ignored and removed on restart); a
// failed write or rename removes the temp file.
bool atomic_write(const fs::path& path, const std::string& content, std::string& err) {
    fs::path tmp = path;
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) {
        err = "cannot create " + tmp.string();
        return false;
    }
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (_commit(_fileno(f)) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        err = "write failed for " + tmp.string();
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    // std::filesystem::rename replaces an existing target (MinGW libstdc++ uses
    // MoveFileExW with MOVEFILE_REPLACE_EXISTING), so there is no remove-first
    // window in which a crash would leave neither the old nor the new file.
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        err = "rename " + tmp.string() + " -> " + path.string() + ": " + ec.message();
        std::error_code rec;
        fs::remove(tmp, rec);
        return false;
    }
    return true;
}

} // namespace detail

bool write_file_atomic(const std::string& path, const std::string& content, std::string& err) {
    return atomic_write(fs::path(path), content, err);
}

} // namespace collatz::checkpoint
