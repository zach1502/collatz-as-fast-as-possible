#ifndef COLLATZ_WORKER_DISPLAY_HPP
#define COLLATZ_WORKER_DISPLAY_HPP

// What `collatz worker` shows while it runs: the progress of a block (from
// verify --progress-file, collatz/progress.hpp), the lines of the display it
// redraws in place on a terminal, the line it prints once a minute otherwise,
// the ANSI text of a redraw, the --status summary (/api/stats), and the
// terminal checks. All but the terminal functions are pure, for the tests.
//
// C++20, with the Windows console API (MinGW-w64) for the terminal calls.

#include "collatz/checkpoint.hpp"
#include "collatz/progress.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace collatz::worker {

/// Progress of one block, from its progress line.
struct BlockProgress {
    double fraction = 0.0;        ///< of the block's numbers, 0..1
    double session_numbers = 0.0; ///< numbers covered since this session started the block
    double speed = 0.0;           ///< session_numbers per second; 0 = not known yet
    double eta_s = -1.0;          ///< seconds left at that speed; < 0 = not known
};

/// fraction = (slices_done + slice_fraction) / slices_total, with the finished
/// share of the slice in progress from progress::slice_fraction (survivors
/// finished over their estimated total), and at least `shown`, the fraction
/// shown before for this block, so that the bar never moves back. Slices
/// finished before this session (slices_resumed) count in the fraction but not
/// in session_numbers, so the speed of a resumed block is this session's own.
/// The block has 2^`block_bits` numbers; `elapsed_s` is the time since this
/// session started the block's verify run.
[[nodiscard]] BlockProgress block_progress(const progress::Snapshot& s,
                                           unsigned block_bits,
                                           double elapsed_s,
                                           double shown = 0.0) noexcept;

/// Everything the display shows. Times of day and texts come formatted; the
/// renderer formats the numbers and durations.
struct DisplayState {
    std::string device; ///< "GPU NVIDIA GeForce RTX 3070", "GPU" or "CPU, 10 threads"
    double uptime_s = 0.0;
    std::string server;
    bool running = false; ///< a block's verify run is in progress
    std::string status;   ///< what the worker does while no block runs
    std::uint64_t unit_id = 0;
    std::string block; ///< block_label of the block in progress
    unsigned block_bits = 0;
    std::string started;        ///< local time this session started the block, "09:12"
    bool have_progress = false; ///< a progress line has been read
    BlockProgress progress;
    std::uint64_t session_blocks = 0; ///< units submitted in this session
    double session_numbers = 0.0; ///< numbers this session covered, the block in progress included
    std::string session_since;    ///< local time the session started
    bool have_frontier = false;
    double frontier_log2 = 0.0;
    std::string frontier_value;   ///< decimal; empty when the reply had none
    double frontier_age_s = 0.0;  ///< seconds since the frontier was fetched
    bool frontier_failed = false; ///< the last fetch failed
    std::string last;             ///< the last submitted unit, in words; empty until one is
    std::string hint;             ///< bottom line: how to pause, or why the worker stopped
};

struct RenderOptions {
    unsigned width = 80; ///< terminal columns
    bool utf8 = false;   ///< block characters in the bar; ASCII otherwise
};

/// The display's lines (at most 9), plain text without escape codes, each cut
/// to width - 1 columns so that none wraps (a wrapped line would break the
/// cursor-up count of the redraw).
[[nodiscard]] std::vector<std::string> render(const DisplayState& s, const RenderOptions& o);

/// One ASCII line on the block in progress (or the status), for output that is
/// not a terminal and for worker.log. It starts with "worker:", never with
/// "unit <id> ", which starts the line of a submitted unit.
[[nodiscard]] std::string plain_progress_line(const DisplayState& s);

/// ANSI text that replaces the `previous` lines just above the cursor with
/// `lines`, leaving the cursor at the start of the line below them.
[[nodiscard]] std::string redraw(std::size_t previous, const std::vector<std::string>& lines);

/// ANSI text that clears the `previous` lines above the cursor and leaves the
/// cursor at the first of them; empty when `previous` is 0.
[[nodiscard]] std::string erase(std::size_t previous);

/// Rows that `lines` take on a terminal `width` columns wide: a line wider
/// than the window wraps, when drawn and when Windows Terminal or conhost
/// rewrap the screen after a resize. (A terminal that cuts the lines on a
/// resize instead makes the count too high, which only clears a few lines
/// above the display.)
[[nodiscard]] std::size_t screen_rows(const std::vector<std::string>& lines, unsigned width);

/// Writes `s` to stdout after flushing stdout and stderr: in one WriteFile
/// call on the output handle, so the C runtime neither translates it nor
/// splits a UTF-8 character between two writes; through fwrite when there is
/// no output handle, or for the unwritten rest when a write fails.
void write_terminal(std::string_view s);

/// "42s", "8m 05s", "3h 12m", "2d 04h"; "--" for a negative or NaN value.
[[nodiscard]] std::string format_duration(double seconds);

/// "0" to "9999", then "1.52e13"; "--" for a negative, infinite or NaN value.
[[nodiscard]] std::string format_count(double x);

/// The start H * 2^K of block `h` (decimal) as "2^e + j * 2^K" with 2^e the
/// highest power of two at or below it: "2^71 + 37 * 2^54", "2^71".
[[nodiscard]] std::string block_label(std::string_view h, unsigned block_bits);

/// UTF-8 code points of `s` (one column each for the text the display uses).
[[nodiscard]] std::size_t columns(std::string_view s) noexcept;

/// `s` cut to `width` columns; a cut line ends in "...".
[[nodiscard]] std::string fit_width(std::string_view s, unsigned width);

/// A bar of `cells` columns for `fraction` (clamped to 0..1): full block, an
/// eighth block and light shade characters with `utf8`, else "[###---]" (two
/// more columns for the brackets). Full only at 1.
[[nodiscard]] std::string progress_bar(double fraction, unsigned cells, bool utf8);

/// Local time of `t` as "09:12", or "2026-10-09 09:12:05" with `with_date`.
[[nodiscard]] std::string local_time_text(std::time_t t, bool with_date);

/// What `collatz worker --status` prints from a /api/stats reply: the
/// campaign, the frontier, the numbers checked, the speed over the last 24
/// hours and the machines. Server text goes through printable().
[[nodiscard]] std::vector<std::string> stats_summary(const checkpoint::Json& stats);

/// frontier.log2 and frontier.value (decimal, through printable) of a
/// /api/stats reply; false without the log2.
[[nodiscard]] bool stats_frontier(const checkpoint::Json& stats, double& log2, std::string& value);

/// A decimal number n > 0 as "2^e + r" with 2^e the highest power of two at
/// or below it and r in format_count form: "2^71 + 2.52e17", "2^71". The
/// text unchanged when it is not a decimal below 2^128.
[[nodiscard]] std::string power_offset_label(std::string_view decimal);

/// NAME of the "gpu device: NAME" line verify --gpu prints, searched in a
/// verify log and passed through printable(); empty when there is none.
[[nodiscard]] std::string gpu_device_from_log(std::string_view log);

/// What stdout is.
struct Terminal {
    bool ansi = false;     ///< a terminal that takes ANSI escapes
    bool utf8 = false;     ///< it shows UTF-8
    bool msys_pty = false; ///< Windows: an MSYS2/Cygwin pty (mintty), where Ctrl-C may not reach
                           ///< native programs
};

/// A console, where VT processing is turned on (when that fails, as on
/// consoles before Windows 10, the result is not ansi) and the output code
/// page is set to UTF-8; or an MSYS2/Cygwin pty pipe, UTF-8 from the locale
/// variables. `ascii` (COLLATZ_ASCII=1) keeps utf8 false and the code page as
/// it is.
[[nodiscard]] Terminal open_terminal(bool ascii);

/// Puts back the console mode and output code page open_terminal changed.
void restore_terminal() noexcept;

/// Columns of the terminal on stdout; COLUMNS, else 80, when it does not say
/// (an MSYS2 pty cannot be asked from a native program).
[[nodiscard]] unsigned terminal_width() noexcept;

/// Rows of the terminal on stdout; 0 when it does not say.
[[nodiscard]] unsigned terminal_height() noexcept;

/// Whether a pipe's file name (GetFileInformationByHandleEx FileNameInfo) is
/// an MSYS2 or Cygwin pty: "\msys-<id>-pty<n>-to-master" or "\cygwin-...-pty...",
/// the test git for Windows uses.
[[nodiscard]] bool is_msys_pty_name(std::wstring_view name) noexcept;

/// Whether the first set of LC_ALL, LC_CTYPE and LANG names a UTF-8 locale.
[[nodiscard]] bool
locale_is_utf8(const char* lc_all, const char* lc_ctype, const char* lang) noexcept;

} // namespace collatz::worker

#endif // COLLATZ_WORKER_DISPLAY_HPP
