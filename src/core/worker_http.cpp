// HTTP through the system curl executable: the rule for server URLs, curl's
// header dump (status), the owner-only request files and http_request.

#include "collatz/version.hpp"
#include "collatz/worker.hpp"
#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

namespace collatz::worker {

namespace fs = std::filesystem;
using namespace detail;

namespace {

/// `s` starts with `lower_prefix`, ASCII case-insensitively.
bool starts_with_ci(std::string_view s, std::string_view lower_prefix) noexcept {
    if (s.size() < lower_prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lower_prefix.size(); ++i) {
        const char c = s[i];
        const char lc = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lc != lower_prefix[i]) {
            return false;
        }
    }
    return true;
}

/// True for http:// URLs to this machine (localhost, 127.0.0.1, [::1]).
bool is_local_http(std::string_view url) noexcept {
    for (const std::string_view local : {"http://localhost", "http://127.0.0.1", "http://[::1]"}) {
        if (starts_with_ci(url, local)) {
            const char next = url.size() > local.size() ? url[local.size()] : '\0';
            if (next == '\0' || next == ':' || next == '/') {
                return true;
            }
        }
    }
    return false;
}

bool all_of_set(std::string_view s, std::string_view set) {
    return std::all_of(
        s.begin(), s.end(), [set](char c) { return set.find(c) != std::string_view::npos; });
}

/// Whether `name` is one of the names http_request gives its files,
/// <kind>-<pid>-<seq>-<tag>.<ext>: kind req or resp with ext json, or hdr or
/// err with ext txt, and tag random_hex(4), 8 lowercase hex digits (empty only
/// if the OS random source failed). Sets `pid`.
bool request_file_name(std::string_view name, long long& pid) {
    static constexpr std::string_view kDigits = "0123456789";
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const std::string_view ext = name.substr(dot + 1);
    std::string_view rest = name.substr(0, dot);
    std::string_view part[4];
    for (int i = 0; i < 3; ++i) {
        const std::size_t dash = rest.find('-');
        if (dash == std::string_view::npos) {
            return false;
        }
        part[i] = rest.substr(0, dash);
        rest.remove_prefix(dash + 1);
    }
    part[3] = rest;
    const bool json = part[0] == "req" || part[0] == "resp";
    const auto number = [](std::string_view s) {
        return !s.empty() && s.size() <= 18 && all_of_set(s, kDigits);
    };
    if (!(json || part[0] == "hdr" || part[0] == "err") || ext != (json ? "json" : "txt") ||
        !number(part[1]) || !number(part[2]) ||
        !(part[3].empty() || (part[3].size() == 8 && all_of_set(part[3], "0123456789abcdef")))) {
        return false;
    }
    pid = std::strtoll(std::string(part[1]).c_str(), nullptr, 10);
    return true;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): every substr starts at most at size()
int parse_http_status(std::string_view headers) noexcept {
    int status = 0;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        std::size_t end = headers.find('\n', pos);
        if (end == std::string_view::npos) {
            end = headers.size();
        }
        const std::string_view line = headers.substr(pos, end - pos);
        if (line.substr(0, 5) == "HTTP/") {
            const std::size_t sp = line.find(' ');
            if (sp != std::string_view::npos && sp + 4 <= line.size()) {
                int v = 0;
                bool ok = true;
                for (std::size_t i = sp + 1; i < sp + 4; ++i) {
                    if (line[i] < '0' || line[i] > '9') {
                        ok = false;
                        break;
                    }
                    v = v * 10 + (line[i] - '0');
                }
                if (ok) {
                    status = v;
                }
            }
        }
        pos = end + 1;
    }
    return status;
}

bool server_url_ok(std::string_view url) noexcept {
    if (url.find_first_of(" \t\r\n\"'\\") != std::string_view::npos) {
        return false;
    }
    if (starts_with_ci(url, "https://")) {
        return url.size() > 8 && url[8] != '/';
    }
    return is_local_http(url);
}

HttpResponse http_request(const std::string& curl,
                          const std::string& method,
                          const std::string& url,
                          const std::string& body,
                          const fs::path& tmp_dir,
                          unsigned timeout_s) {
    HttpResponse res;
    std::string derr;
    (void) make_private_dirs(tmp_dir, derr); // a failure shows up when the files are written
    static std::atomic<unsigned> seq{0};
    const std::string tag =
        std::to_string(current_pid()) + "-" + std::to_string(++seq) + "-" + random_hex(4);
    const fs::path req = tmp_dir / ("req-" + tag + ".json");
    const fs::path out = tmp_dir / ("resp-" + tag + ".json");
    const fs::path hdr = tmp_dir / ("hdr-" + tag + ".txt");
    const fs::path log = tmp_dir / ("err-" + tag + ".txt");
    const auto cleanup = [&] {
        for (const fs::path& p : {req, out, hdr, log}) {
            std::error_code e2;
            fs::remove(p, e2);
        }
    };
    // -q must come first: it stops curl from reading a .curlrc, which could add
    // options (a proxy, -L, an output file) to every request.
    std::vector<std::string> argv = {curl,
                                     "-q",
                                     "-sS",
                                     "--proto",
                                     is_local_http(url) ? "=http" : "=https",
                                     "--proto-redir",
                                     "=https",
                                     "--max-time",
                                     std::to_string(timeout_s),
                                     "--connect-timeout",
                                     "30",
                                     "--max-filesize",
                                     "16777216",
                                     "-A",
                                     std::string("collatz-worker/") +
                                         std::string(collatz::version_string()),
                                     "-H",
                                     "Accept: application/json",
                                     "-o",
                                     out.string(),
                                     "-D",
                                     hdr.string()};
    if (method == "POST") {
        // The body (which holds the worker token) goes through an owner-only file.
        std::string werr;
        if (!create_private_file(req, body, false, werr)) {
            cleanup();
            res.error = "cannot write a request file: " + werr;
            return res;
        }
        argv.insert(argv.end(),
                    {"-H", "Content-Type: application/json", "--data-binary", "@" + req.string()});
    }
    argv.push_back(url);
    ProcessOptions po;
    po.stdout_path = log.string();
    po.stderr_path = log.string();
    std::string perr;
    const int rc = run_process(argv, po, perr);
    std::string headers;
    std::string message;
    (void) read_text(hdr, headers);
    (void) read_text(out, res.body);
    (void) read_text(log, message);
    cleanup();
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    if (rc != 0) {
        res.body.clear();
        // Only a start failure sets perr; a Windows exit code may be negative.
        res.error = !perr.empty() ? perr
                                  : "curl exited with " + exit_code_text(rc) +
                                        (message.empty() ? std::string() : ": " + message);
        return res;
    }
    res.status = parse_http_status(headers);
    if (res.status == 0) {
        res.error = "no HTTP status line in curl's response";
    }
    return res;
}

std::size_t remove_stale_request_files(const fs::path& tmp_dir) {
    std::size_t removed = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(tmp_dir, ec)) {
        long long pid = 0;
        if (!request_file_name(e.path().filename().string(), pid) ||
            (pid > 0 && process_alive(pid))) {
            continue; // not a file http_request wrote, or one of a live process
        }
        std::error_code e2;
        if (fs::remove(e.path(), e2)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace collatz::worker
