// The config file of the worker client: its default path, load_config,
// save_config through an owner-only temp file, random hex ids, and the worker
// token file.

#include "collatz/worker.hpp"
#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace collatz::worker {

namespace fs = std::filesystem;
using checkpoint::Json;
using namespace detail;

namespace detail {

bool read_text(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !in.bad();
}

} // namespace detail

namespace {

bool is_hex(std::string_view s, std::size_t min_len, std::size_t max_len) {
    if (s.size() < min_len || s.size() > max_len) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

/// Replaces `p` with `content` through a new owner-only temp file in the same
/// directory and a rename, so the file is never readable by others, not even
/// before the rename, and a crash leaves the old or the new file.
bool write_private_file(const fs::path& p, const std::string& content, std::string& err) {
    const std::string suffix = ".tmp-" + std::to_string(current_pid()) + "-" + random_hex(4);
    const fs::path tmp = p.parent_path() / (p.filename().string() + suffix);
    if (!create_private_file(tmp, content, true, err)) {
        return false;
    }
    if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        0) {
        err = "cannot replace " + p.string() + " (Windows error " + std::to_string(GetLastError()) +
              ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

/// Overwrites the bytes of `s` and empties it, for strings that held a token.
void wipe(std::string& s) noexcept {
    // Through a volatile pointer so the stores are not dropped as dead.
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); ++i) {
        p[i] = '\0';
    }
    s.clear();
}

} // namespace

fs::path default_config_path() {
    const wchar_t* app = _wgetenv(L"APPDATA");
    if (app == nullptr || *app == L'\0') {
        return {};
    }
    const std::string base = ascii_path(app);
    if (base.empty()) {
        return {};
    }
    return fs::path(base) / "collatz" / "worker.json";
}

bool load_config(const fs::path& p, Config& out, std::string& err) {
    out = Config{};
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        return true;
    }
    std::string text;
    if (!read_text(p, text)) {
        err = "cannot read " + p.string();
        return false;
    }
    std::string perr;
    const std::optional<Json> doc = checkpoint::parse_json(text, &perr);
    if (!doc || doc->type != Json::Type::Object) {
        err = p.string() + " is not a JSON object (" + perr + ")";
        return false;
    }
    const auto str = [&](const char* key, std::string& dst) {
        const Json* v = doc->get(key);
        if (v != nullptr && v->type == Json::Type::String) {
            dst = v->text;
        }
    };
    str("host_id", out.host_id);
    str("server", out.server);
    if (!out.host_id.empty() && !is_hex(out.host_id, 16, 64)) {
        err = p.string() + ": host_id is not 16-64 lowercase hex digits";
        return false;
    }
    return true;
}

bool save_config(const fs::path& p, const Config& c, std::string& err) {
    if (!make_private_dirs(p.parent_path(), err)) {
        return false;
    }
    std::string s = "{\n  \"schema\": \"collatz-worker-config/1\",\n";
    s += "  \"host_id\": " + json_quote(c.host_id) + ",\n";
    s += "  \"server\": " + json_quote(c.server) + "\n}\n";
    return write_private_file(p, s, err);
}

std::string random_hex(std::size_t bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string raw(bytes, '\0');
    if (!random_bytes(reinterpret_cast<unsigned char*>(raw.data()), bytes)) {
        return {};
    }
    std::string out;
    out.reserve(bytes * 2);
    for (const char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        out += kHex[u >> 4];
        out += kHex[u & 0xFu];
    }
    return out;
}

bool valid_worker_token(std::string_view t) noexcept {
    return t.size() >= 32 && t.size() <= 1024 &&
           std::all_of(t.begin(), t.end(), [](char c) { return c > 0x20 && c < 0x7f; });
}

bool read_worker_token(const fs::path& p, std::string& token, std::string& err) {
    token.clear();
    std::string text;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || !read_text(p, text)) {
        err = "cannot read the token file " + p.string();
        return false;
    }
    std::string_view v = text;
    if (v.substr(0, 3) == "\xEF\xBB\xBF") {
        v.remove_prefix(3);
    }
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!v.empty() && blank(v.front())) {
        v.remove_prefix(1);
    }
    while (!v.empty() && blank(v.back())) {
        v.remove_suffix(1);
    }
    const bool ok = valid_worker_token(v);
    if (ok) {
        token.assign(v);
    } else {
        err = p.string() +
              " does not hold a worker token (one line of 32 to 1024 printable characters without "
              "spaces)";
    }
    wipe(text);
    return ok;
}

} // namespace collatz::worker
