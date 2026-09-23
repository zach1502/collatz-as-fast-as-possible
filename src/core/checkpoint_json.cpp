// The JSON value type and the parser of the files this program writes.

#include "collatz/checkpoint.hpp"

namespace collatz::checkpoint {

const Json* Json::get(std::string_view key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& kv : obj) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse_document(Json& out, std::string& err) {
        if (!value(out, 0)) {
            err = err_.empty() ? "invalid JSON" : err_;
            return false;
        }
        ws();
        if (p_ != s_.size()) {
            err = "trailing data after JSON value at offset " + std::to_string(p_);
            return false;
        }
        return true;
    }

private:
    std::string_view s_;
    std::size_t p_ = 0;
    std::string err_;

    bool fail(const char* what) {
        if (err_.empty()) {
            err_ = std::string(what) + " at offset " + std::to_string(p_);
        }
        return false;
    }
    void ws() {
        while (p_ < s_.size() &&
               (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) {
            ++p_;
        }
    }
    bool lit(std::string_view w) {
        if (s_.substr(p_, w.size()) == w) {
            p_ += w.size();
            return true;
        }
        return false;
    }
    bool string(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') {
            return fail("expected string");
        }
        ++p_;
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (p_ >= s_.size()) {
                    return fail("bad escape");
                }
                const char e = s_[p_++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'u': {
                    // The writer emits \u only for control characters, so only
                    // ASCII code points are accepted.
                    if (p_ + 4 > s_.size()) {
                        return fail("bad \\u escape");
                    }
                    unsigned v = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[p_++];
                        v <<= 4;
                        if (h >= '0' && h <= '9') {
                            v |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            v |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            v |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return fail("bad \\u escape");
                        }
                    }
                    out += static_cast<char>(v < 0x80 ? v : '?');
                    break;
                }
                default:
                    return fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return fail("unterminated string");
    }
    bool value(Json& v, int depth) {
        if (depth > 64) {
            return fail("nesting too deep");
        }
        ws();
        if (p_ >= s_.size()) {
            return fail("unexpected end of input");
        }
        const char c = s_[p_];
        if (c == '{') {
            ++p_;
            v.type = Json::Type::Object;
            ws();
            if (p_ < s_.size() && s_[p_] == '}') {
                ++p_;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                ws();
                if (p_ >= s_.size() || s_[p_] != ':') {
                    return fail("expected ':'");
                }
                ++p_;
                Json child;
                if (!value(child, depth + 1)) {
                    return false;
                }
                v.obj.emplace_back(std::move(key), std::move(child));
                ws();
                if (p_ < s_.size() && s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < s_.size() && s_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p_;
            v.type = Json::Type::Array;
            ws();
            if (p_ < s_.size() && s_[p_] == ']') {
                ++p_;
                return true;
            }
            for (;;) {
                Json child;
                if (!value(child, depth + 1)) {
                    return false;
                }
                v.arr.push_back(std::move(child));
                ws();
                if (p_ < s_.size() && s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < s_.size() && s_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Json::Type::String;
            return string(v.text);
        }
        if (lit("true")) {
            v.type = Json::Type::Bool;
            v.b = true;
            return true;
        }
        if (lit("false")) {
            v.type = Json::Type::Bool;
            v.b = false;
            return true;
        }
        if (lit("null")) {
            v.type = Json::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const std::size_t start = p_;
            ++p_;
            while (p_ < s_.size() &&
                   ((s_[p_] >= '0' && s_[p_] <= '9') || s_[p_] == '.' || s_[p_] == 'e' ||
                    s_[p_] == 'E' || s_[p_] == '+' || s_[p_] == '-')) {
                ++p_;
            }
            v.type = Json::Type::Number;
            v.text = std::string(s_.substr(start, p_ - start));
            return true;
        }
        return fail("unexpected character");
    }
};

} // namespace

std::optional<Json> parse_json(std::string_view src, std::string* err) {
    Json v;
    std::string e;
    Parser p(src);
    if (!p.parse_document(v, e)) {
        if (err != nullptr) {
            *err = e;
        }
        return std::nullopt;
    }
    return v;
}

} // namespace collatz::checkpoint
