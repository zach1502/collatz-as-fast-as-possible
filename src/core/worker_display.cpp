// The lines of the `collatz worker` display (render) and of the --status
// summary (stats_summary, stats_frontier).

#include "collatz/worker_display.hpp"

#include "collatz/worker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace collatz::worker {

using checkpoint::Json;

namespace {

/// Column of the values after the labels ("frontier " is the longest label).
constexpr std::size_t kLabel = 9;

std::string label(const char* name) {
    std::string s = name;
    s.resize(std::max(s.size() + 1, kLabel), ' ');
    return s;
}

std::string ago(double s) {
    if (!(s >= 0.0)) {
        return "at an unknown time";
    }
    const auto t = static_cast<unsigned long long>(std::min(s, 1e12));
    if (t < 60) {
        return "just now";
    }
    if (t < 3600) {
        return std::to_string(t / 60) + " min ago";
    }
    if (t < 2 * 86400) {
        return std::to_string(t / 3600) + " h ago";
    }
    return std::to_string(t / 86400) + " days ago";
}

std::string percent(double fraction) {
    char b[16];
    std::snprintf(b, sizeof(b), "%5.1f%%", std::clamp(fraction, 0.0, 1.0) * 100.0);
    return b;
}

const Json* member(const Json* o, const char* key) {
    return o != nullptr && o->type == Json::Type::Object ? o->get(key) : nullptr;
}

/// A string or number member as printable text; "" otherwise.
std::string text_of(const Json* v, std::size_t max_len = 100) {
    if (v == nullptr || (v->type != Json::Type::String && v->type != Json::Type::Number)) {
        return {};
    }
    return printable(v->text, max_len);
}

bool number_of(const Json* v, double& out) {
    if (v == nullptr || v->type != Json::Type::Number) {
        return false;
    }
    char* end = nullptr;
    const double x = std::strtod(v->text.c_str(), &end);
    if (end == v->text.c_str() || !std::isfinite(x)) {
        return false;
    }
    out = x;
    return true;
}

} // namespace

std::vector<std::string> render(const DisplayState& s, const RenderOptions& o) {
    const unsigned w = std::max(o.width, 24u) - 1;
    std::vector<std::string> out;
    const auto add = [&](const std::string& line) { out.push_back(fit_width(line, w)); };

    std::string title = "collatz worker";
    if (!s.device.empty()) {
        title += "   " + s.device;
    }
    add(title + "   up " + format_duration(s.uptime_s));
    add(label("server") + s.server);
    double session_speed = 0.0;
    if (s.uptime_s > 0.0) {
        session_speed = s.session_numbers / s.uptime_s;
    }
    if (s.running) {
        std::string b = label("block") + s.block + "  (2^" + std::to_string(s.block_bits) +
                        " numbers, unit " + std::to_string(s.unit_id);
        if (!s.started.empty()) {
            b += ", started " + s.started;
        }
        add(b + ")");
        const std::string tail =
            s.have_progress ? "  " + percent(s.progress.fraction) + "  ETA " +
                                  (s.progress.eta_s >= 0.0 ? format_duration(s.progress.eta_s)
                                                           : std::string("--"))
                            : std::string("  starting");
        // The bar takes what the label and the tail leave, 10 to 40 cells.
        const std::size_t fixed = kLabel + columns(tail) + (o.utf8 ? 0u : 2u);
        const auto cells =
            static_cast<unsigned>(std::clamp<std::size_t>(w > fixed ? w - fixed : 0, 10, 40));
        add(label("progress") + progress_bar(s.progress.fraction, cells, o.utf8) + tail);
        add(label("speed") +
            (s.progress.speed > 0.0 ? format_count(s.progress.speed) + " numbers/s"
                                    : std::string("--")) +
            " this block, " +
            (session_speed > 0.0 ? format_count(session_speed) : std::string("--")) +
            " this session");
    } else {
        add(label("status") + s.status);
        add(label("speed") +
            (session_speed > 0.0 ? format_count(session_speed) + " numbers/s" : std::string("--")) +
            " this session");
    }
    add(label("session") + std::to_string(s.session_blocks) +
        (s.session_blocks == 1 ? " block, " : " blocks, ") + format_count(s.session_numbers) +
        " numbers" + (s.session_since.empty() ? "" : " (since " + s.session_since + ")"));
    if (s.have_frontier) {
        // Four decimals of log2 move slowly at 2^71 (one step is ~1.6e17
        // numbers), so the offset form comes first.
        char b[32];
        std::snprintf(b, sizeof(b), "2^%.4f", s.frontier_log2);
        const std::string exact = power_offset_label(s.frontier_value);
        const std::string where = s.frontier_value.empty()               ? std::string(b)
                                  : exact.find('+') == std::string::npos ? exact
                                                                         : exact + " (" + b + ")";
        add(label("frontier") + where + ", from the server " + ago(s.frontier_age_s) +
            (s.frontier_failed ? "; the last check failed" : ""));
    } else {
        add(label("frontier") +
            (s.frontier_failed ? "not known (the server did not answer)" : "not known yet"));
    }
    if (!s.last.empty()) {
        add(label("last") + s.last);
    }
    if (!s.hint.empty()) {
        add(s.hint);
    }
    return out;
}

std::vector<std::string> stats_summary(const Json& stats) {
    std::vector<std::string> out;
    const Json* c = member(&stats, "campaign");
    if (c == nullptr || c->type != Json::Type::Object) {
        const std::string why = text_of(member(&stats, "message"));
        out.push_back(why.empty() ? "no campaign yet" : why);
        return out;
    }
    char b[96];
    std::string head =
        "campaign " + text_of(member(c, "name"), 64) + " (id " + text_of(member(c, "id"), 20) + ")";
    double from = 0.0;
    if (number_of(member(c, "start_log2"), from)) {
        std::snprintf(b, sizeof(b), ", checking from 2^%.4f", from);
        head += b;
    }
    out.push_back(head);

    const Json* f = member(&stats, "frontier");
    std::string fl = "frontier: " + text_of(member(f, "value"), 60);
    double lg = 0.0;
    if (number_of(member(f, "log2"), lg)) {
        std::snprintf(b, sizeof(b), " (2^%.4f)", lg);
        fl += b;
    }
    out.push_back(fl);

    const Json* t = member(&stats, "totals");
    const std::string checked = text_of(member(t, "numbers_verified"), 60);
    std::string nl = "numbers checked: " + (checked.empty() ? std::string("0") : checked);
    if (checked.size() > 4) { // shorter ones read as they are
        nl += " (" + format_count(std::strtod(checked.c_str(), nullptr)) + ")";
    }
    if (const std::string blocks = text_of(member(t, "blocks"), 20); !blocks.empty()) {
        nl += " in " + blocks + (blocks == "1" ? " block" : " blocks");
    }
    out.push_back(nl);

    const Json* sp = member(&stats, "speed");
    double nps = 0.0;
    if (number_of(member(member(sp, "last_24_h"), "numbers_per_second"), nps)) {
        std::string sl = "speed (24 h): " + format_count(nps) + " numbers/s";
        double per_day = 0.0;
        if (number_of(member(sp, "blocks_per_day"), per_day)) {
            if (per_day < 100.0) {
                std::snprintf(b, sizeof(b), ", %.1f blocks a day", per_day);
            } else {
                std::snprintf(b, sizeof(b), ", %.0f blocks a day", per_day);
            }
            sl += b;
        }
        out.push_back(sl);
    } else {
        out.push_back("speed (24 h): no block accepted in the last 24 hours");
    }

    const Json* hosts = member(&stats, "hosts");
    if (hosts != nullptr && hosts->type == Json::Type::Array) {
        out.push_back("machines: " + std::to_string(hosts->arr.size()));
        double now = 0.0;
        const bool have_now = number_of(member(&stats, "generated_at"), now);
        std::size_t shown = 0;
        for (const Json& h : hosts->arr) {
            if (++shown > 10) {
                out.push_back("  and " + std::to_string(hosts->arr.size() - 10) + " more");
                break;
            }
            // Host ids are 32 hex digits; 8 tell the machines apart.
            const std::string blocks = text_of(member(&h, "blocks"), 20);
            std::string line =
                "  " + text_of(member(&h, "host"), 32).substr(0, 8) + "  " +
                text_of(member(&h, "backend"), 8) + "  " + blocks +
                (blocks == "1" ? " block, " : " blocks, ") +
                format_count(std::strtod(text_of(member(&h, "numbers"), 60).c_str(), nullptr)) +
                " numbers";
            double hnps = 0.0;
            if (number_of(member(&h, "numbers_per_second"), hnps)) {
                line += ", " + format_count(hnps) + " numbers/s";
            }
            double last_at = 0.0;
            if (have_now && number_of(member(&h, "last_at"), last_at)) {
                line += ", last block " + ago(now - last_at);
            }
            out.push_back(line);
        }
    }
    double alerts = 0.0;
    if (number_of(member(&stats, "alerts_open"), alerts) && alerts > 0.0) {
        out.push_back("alerts: " + text_of(member(&stats, "alerts_open"), 20) + " open");
    }
    return out;
}

bool stats_frontier(const Json& stats, double& log2, std::string& value) {
    const Json* f = member(&stats, "frontier");
    if (!number_of(member(f, "log2"), log2)) {
        return false;
    }
    value = text_of(member(f, "value"), 60);
    return true;
}

} // namespace collatz::worker
