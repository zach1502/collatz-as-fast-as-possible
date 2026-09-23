// JSON text helpers of the worker client (string literals, compact dumps,
// string members), the worker token kept out of printed text, and server text
// made printable.

#include "collatz/worker.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace collatz::worker {

using checkpoint::Json;

namespace {

constexpr std::string_view kTokenMark = "[token]";
constexpr std::string_view kRedactedMark = "[redacted]";

// The registered worker token, and its text inside a JSON string when that
// differs (a token may hold '"' or '\').
struct Secrets {
    std::mutex m;
    std::vector<std::string> forms;
};

Secrets& secrets() {
    static Secrets s;
    return s;
}

void replace_all(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return;
    }
    for (std::size_t at = s.find(from); at != std::string::npos;
         at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
}

bool name_at(const std::string& s, std::size_t at, std::string_view lower) {
    if (at + lower.size() > s.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lower.size(); ++i) {
        const char c = s[at + i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != lower[i]) {
            return false;
        }
    }
    return true;
}

// The value after each "authorization" name that a ':' or '=' follows (a
// header line, or a JSON or form field), up to the end of the line or a quote.
void redact_authorization(std::string& s) {
    static constexpr std::string_view kName = "authorization";
    const auto quote = [&s](std::size_t i) {
        return i < s.size() && (s[i] == '"' || s[i] == '\'');
    };
    const auto blanks = [&s](std::size_t i) {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
        return i;
    };
    std::size_t i = 0;
    while (i < s.size()) {
        if (!name_at(s, i, kName)) {
            ++i;
            continue;
        }
        std::size_t v = i + kName.size();
        if (quote(v)) {
            ++v;
        }
        v = blanks(v);
        if (v >= s.size() || (s[v] != ':' && s[v] != '=')) {
            i += kName.size();
            continue;
        }
        v = blanks(v + 1);
        if (quote(v)) {
            ++v;
        }
        std::size_t end = v;
        while (end < s.size() && s[end] != '\r' && s[end] != '\n' && !quote(end)) {
            ++end;
        }
        s.replace(v, end - v, kRedactedMark);
        i = v + kRedactedMark.size();
    }
}

} // namespace

std::string json_quote(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            out += "\\u00";
            out += kHex[u >> 4];
            out += kHex[u & 0xFu];
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::string string_member(const Json* obj, const char* key) {
    const Json* v = obj != nullptr ? obj->get(key) : nullptr;
    return v != nullptr && v->type == Json::Type::String ? v->text : std::string();
}

std::string json_dump(const Json& v) {
    switch (v.type) {
    case Json::Type::Null:
        return "null";
    case Json::Type::Bool:
        return v.b ? "true" : "false";
    case Json::Type::Number:
        return v.text;
    case Json::Type::String:
        return json_quote(v.text);
    case Json::Type::Array: {
        std::string s = "[";
        for (std::size_t i = 0; i < v.arr.size(); ++i) {
            s += (i == 0 ? "" : ",") + json_dump(v.arr[i]);
        }
        return s + "]";
    }
    case Json::Type::Object: {
        std::string s = "{";
        for (std::size_t i = 0; i < v.obj.size(); ++i) {
            s +=
                (i == 0 ? "" : ",") + json_quote(v.obj[i].first) + ":" + json_dump(v.obj[i].second);
        }
        return s + "}";
    }
    }
    return "null";
}

void set_secret(std::string_view secret) {
    Secrets& sec = secrets();
    const std::lock_guard<std::mutex> lk(sec.m);
    sec.forms.clear();
    if (secret.empty()) {
        return;
    }
    sec.forms.emplace_back(secret);
    const std::string quoted = json_quote(secret);
    const std::string_view inner = std::string_view(quoted).substr(1, quoted.size() - 2);
    if (inner != secret) {
        sec.forms.emplace_back(inner);
    }
}

std::string redact(std::string_view s) {
    std::string out(s);
    {
        Secrets& sec = secrets();
        const std::lock_guard<std::mutex> lk(sec.m);
        for (const std::string& form : sec.forms) {
            replace_all(out, form, kTokenMark);
        }
    }
    redact_authorization(out);
    return out;
}

std::string printable(std::string_view s, std::size_t max_len) {
    // Redacted before the cut, so that a cut cannot leave part of the token.
    const std::string text = redact(s);
    const std::string_view r = text;
    std::string out;
    out.reserve(std::min(r.size(), max_len) + 3);
    for (const char c : r.substr(0, std::min(r.size(), max_len))) {
        out += (c >= 0x20 && c <= 0x7e) ? c : '?';
    }
    if (r.size() > max_len) {
        out += "...";
    }
    return out;
}

} // namespace collatz::worker
