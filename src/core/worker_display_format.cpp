// The progress of a block and the text the display is built from: durations,
// counts, block and power-of-two labels, text cut to the width, the bar, the
// plain progress line, the ANSI text of a redraw and the GPU name in a log.

#include "collatz/u128.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace collatz::worker {

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

} // namespace

BlockProgress block_progress(const progress::Snapshot& s,
                             unsigned block_bits,
                             double elapsed_s,
                             double shown) noexcept {
    BlockProgress p;
    const std::uint64_t total = std::max<std::uint64_t>(s.slices_total, 1);
    const double slices = static_cast<double>(total);
    // A finished run keeps the counts of its last slice; they are not a slice
    // in progress.
    const double in_slice = s.slices_done < total ? progress::slice_fraction(s) : 0.0;
    p.fraction = std::clamp((static_cast<double>(s.slices_done) + in_slice) / slices, 0.0, 1.0);
    // The survivor total is an estimate until Step 1 ends; a rise in it must
    // not move the bar back.
    p.fraction = std::max(p.fraction, std::clamp(shown, 0.0, 1.0));
    const double numbers = std::ldexp(1.0, static_cast<int>(block_bits));
    const double resumed = static_cast<double>(s.slices_resumed) / slices;
    p.session_numbers = std::max(0.0, p.fraction - resumed) * numbers;
    if (elapsed_s > 0.0 && p.session_numbers > 0.0) {
        p.speed = p.session_numbers / elapsed_s;
        p.eta_s = (1.0 - p.fraction) * numbers / p.speed;
    }
    return p;
}

std::string format_duration(double seconds) {
    if (!(seconds >= 0.0)) {
        return "--";
    }
    // 10^12 s caps the cast; an estimate beyond that is not useful.
    // NOLINTNEXTLINE(bugprone-incorrect-roundings): seconds >= 0, so this rounds half up
    const auto t = static_cast<unsigned long long>(std::min(seconds, 1e12) + 0.5);
    char b[48];
    if (t < 60) {
        std::snprintf(b, sizeof(b), "%llus", t);
    } else if (t < 3600) {
        std::snprintf(b, sizeof(b), "%llum %02llus", t / 60, t % 60);
    } else if (t < 86400) {
        std::snprintf(b, sizeof(b), "%lluh %02llum", t / 3600, (t / 60) % 60);
    } else {
        std::snprintf(b, sizeof(b), "%llud %02lluh", t / 86400, (t / 3600) % 24);
    }
    return b;
}

std::string format_count(double x) {
    if (!(x >= 0.0) || !std::isfinite(x)) {
        return "--";
    }
    char b[48];
    if (x < 9999.5) {
        std::snprintf(b, sizeof(b), "%.0f", x);
        return b;
    }
    int e = static_cast<int>(std::floor(std::log10(x)));
    double m = x / std::pow(10.0, e);
    if (m >= 9.995) { // would print as 10.00
        m /= 10.0;
        ++e;
    }
    std::snprintf(b, sizeof(b), "%.2fe%d", m, e);
    return b;
}

std::string block_label(std::string_view h, unsigned block_bits) {
    u128 H = 0;
    if (!from_string(h, H)) {
        return printable(h, 40) + " * 2^" + std::to_string(block_bits);
    }
    if (H == 0) {
        return "0";
    }
    const int top = bit_length(H) - 1;
    const u128 j = H - (static_cast<u128>(1) << top);
    std::string s = "2^" + std::to_string(static_cast<unsigned>(top) + block_bits);
    if (j != 0) {
        s += " + " + to_string(j) + " * 2^" + std::to_string(block_bits);
    }
    return s;
}

std::size_t columns(std::string_view s) noexcept {
    std::size_t n = 0;
    for (const char c : s) {
        n += (static_cast<unsigned char>(c) & 0xC0u) != 0x80u ? 1u : 0u;
    }
    return n;
}

std::string fit_width(std::string_view s, unsigned width) {
    if (columns(s) <= width) {
        return std::string(s);
    }
    const std::size_t keep = width > 3 ? width - 3 : width;
    std::size_t cps = 0;
    std::size_t i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u) {
            if (cps == keep) {
                break;
            }
            ++cps;
        }
    }
    return std::string(s.substr(0, i)) + (width > 3 ? "..." : "");
}

std::string progress_bar(double fraction, unsigned cells, bool utf8) {
    const double f = std::isnan(fraction) ? 0.0 : std::clamp(fraction, 0.0, 1.0);
    if (!utf8) {
        const auto full = static_cast<unsigned>(f * cells);
        return "[" + std::string(full, '#') + std::string(cells - full, '-') + "]";
    }
    // U+2588 FULL BLOCK, U+258F..U+2589 the left one to seven eighths, U+2591
    // LIGHT SHADE for the rest.
    static constexpr const char* kEighths[8] = {"",
                                                "\xE2\x96\x8F",
                                                "\xE2\x96\x8E",
                                                "\xE2\x96\x8D",
                                                "\xE2\x96\x8C",
                                                "\xE2\x96\x8B",
                                                "\xE2\x96\x8A",
                                                "\xE2\x96\x89"};
    const auto eighths = static_cast<unsigned>(f * cells * 8.0);
    const unsigned full = eighths / 8;
    const unsigned part = full < cells ? eighths % 8 : 0;
    std::string s;
    for (unsigned i = 0; i < full; ++i) {
        s += "\xE2\x96\x88";
    }
    s += kEighths[part];
    for (unsigned i = full + (part != 0 ? 1u : 0u); i < cells; ++i) {
        s += "\xE2\x96\x91";
    }
    return s;
}

std::string plain_progress_line(const DisplayState& s) {
    if (!s.running) {
        return "worker: " + s.status;
    }
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%.1f%%", std::clamp(s.progress.fraction, 0.0, 1.0) * 100.0);
    std::string line = "worker: unit " + std::to_string(s.unit_id) + " " +
                       (s.have_progress ? std::string(pct) + " done" : std::string("starting")) +
                       " (2^" + std::to_string(s.block_bits) + " numbers at " + s.block + ")";
    if (s.progress.speed > 0.0) {
        line += ", " + format_count(s.progress.speed) + " numbers/s, ETA " +
                format_duration(s.progress.eta_s);
    }
    return line;
}

std::string redraw(std::size_t previous, const std::vector<std::string>& lines) {
    std::string out = "\r";
    if (previous > 0) {
        out += "\x1b[" + std::to_string(previous) + "A";
    }
    // A CR before each line: where LF does not also return the carriage (a
    // console with DISABLE_NEWLINE_AUTO_RETURN, a raw tty), the lines still
    // start in the first column.
    for (const std::string& l : lines) {
        out += "\r\x1b[2K";
        out += l;
        out += '\n';
    }
    out += "\r\x1b[J"; // a longer previous frame leaves lines below
    return out;
}

std::string erase(std::size_t previous) {
    if (previous == 0) {
        return {};
    }
    return "\r\x1b[" + std::to_string(previous) + "A\x1b[J";
}

std::size_t screen_rows(const std::vector<std::string>& lines, unsigned width) {
    std::size_t rows = 0;
    for (const std::string& l : lines) {
        const std::size_t c = columns(l);
        rows += width == 0 || c <= width ? 1 : (c + width - 1) / width;
    }
    return rows;
}

std::string power_offset_label(std::string_view decimal) {
    u128 v = 0;
    if (!from_string(decimal, v) || v == 0) {
        return std::string(decimal);
    }
    const int top = bit_length(v) - 1;
    const u128 r = v - (static_cast<u128>(1) << top);
    std::string s = "2^" + std::to_string(top);
    if (r != 0) {
        s += " + " + format_count(static_cast<double>(r));
    }
    return s;
}

std::string gpu_device_from_log(std::string_view log) {
    constexpr std::string_view kKey = "gpu device: ";
    std::size_t pos = 0;
    while (pos < log.size()) {
        const std::size_t end = log.find('\n', pos);
        if (end == std::string_view::npos) {
            break; // a line still being written
        }
        std::string_view line = log.substr(pos, end - pos);
        if (starts_with(line, kKey)) {
            line.remove_prefix(kKey.size());
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            return printable(line, 80);
        }
        pos = end + 1;
    }
    return {};
}

} // namespace collatz::worker
