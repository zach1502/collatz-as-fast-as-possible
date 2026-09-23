// The progress file: format_line, parse_line, write_line and the FileWriter
// thread.

#include "collatz/progress.hpp"
#include "collatz/u128.hpp"

#include <cstdio>
#include <system_error>
#include <utility>

namespace collatz::progress {

std::string format_line(const Snapshot& s) {
    return std::to_string(s.tasks_done) + ' ' + std::to_string(s.tasks_total) + ' ' +
           std::to_string(s.slices_done) + ' ' + std::to_string(s.slices_total) + ' ' +
           std::to_string(s.slices_resumed) + ' ' + std::to_string(s.survivors_done) + ' ' +
           std::to_string(s.survivors_total) + '\n';
}

bool parse_line(std::string_view text, Snapshot& out) {
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    std::uint64_t v[7] = {};
    std::size_t n = 0;
    while (!text.empty()) {
        if (n == 7) {
            return false;
        }
        const std::size_t sp = text.find(' ');
        const std::string_view tok = text.substr(0, sp);
        u128 x = 0;
        // from_string also takes a leading '+'; the first-character test refuses it.
        if (tok.empty() || tok.size() > 20 || tok.front() < '0' || tok.front() > '9' ||
            !collatz::from_string(tok, x) || x > ~std::uint64_t{0}) {
            return false;
        }
        v[n++] = static_cast<std::uint64_t>(x);
        if (sp == std::string_view::npos) {
            break;
        }
        text.remove_prefix(sp + 1);
        if (text.empty()) {
            return false; // a trailing space
        }
    }
    if (n != 7) {
        return false;
    }
    Snapshot s;
    s.tasks_done = v[0];
    s.tasks_total = v[1];
    s.slices_done = v[2];
    s.slices_total = v[3];
    s.slices_resumed = v[4];
    s.survivors_done = v[5];
    s.survivors_total = v[6];
    if (s.tasks_done > s.tasks_total || s.slices_total == 0 || s.slices_done > s.slices_total ||
        s.slices_resumed > s.slices_done || s.survivors_done > s.survivors_total) {
        return false;
    }
    out = s;
    return true;
}

bool write_line(const std::filesystem::path& path, const std::string& line) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(line.data(), 1, line.size(), f) == line.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) {
        // Replaces an existing file (MinGW's libstdc++ uses MoveFileExW with
        // MOVEFILE_REPLACE_EXISTING).
        std::filesystem::rename(tmp, path, ec);
    }
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

FileWriter::FileWriter(std::filesystem::path path, std::chrono::milliseconds period)
    : path_(std::move(path)), period_(period) {
    (void) write_line(path_, format_line(snapshot()));
    th_ = std::thread([this] { loop(); });
}

FileWriter::~FileWriter() {
    {
        const std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) {
        th_.join();
    }
    // The worker keeps the final counts. On Windows the rename fails while the
    // worker has the file open, which lasts microseconds, so the write is retried.
    const std::string line = format_line(snapshot());
    for (int attempt = 0; attempt < 10 && !write_line(path_, line); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void FileWriter::loop() {
    std::unique_lock<std::mutex> lk(m_);
    while (!cv_.wait_for(lk, period_, [this] { return stop_; })) {
        lk.unlock();
        (void) write_line(path_, format_line(snapshot()));
        lk.lock();
    }
}

} // namespace collatz::progress
