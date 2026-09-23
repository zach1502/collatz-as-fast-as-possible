#ifndef COLLATZ_CLI_CMD_WORKER_DISPLAY_HPP
#define COLLATZ_CLI_CMD_WORKER_DISPLAY_HPP

// The live display's terminal frame (Screen) and the frontier it shows
// (FrontierView), shared by the src/cli/cmd_worker*.cpp pieces.

#include "collatz/worker_display.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace collatz::cli::worker_detail {

namespace wk = collatz::worker;

using Clock = std::chrono::steady_clock;

/// The live display on a terminal: the frame is drawn below everything else
/// and redrawn in place. The worker draws it only while it waits (on verify,
/// on a backoff) and erases it (suspend) when the wait ends, before it prints
/// anything else, so a message is never drawn over and scrolls up above the
/// next frame. When not live it draws nothing.
class Screen {
public:
    void start(bool live, bool utf8) {
        live_ = live;
        utf8_ = utf8;
    }
    [[nodiscard]] bool live() const {
        return live_;
    }
    [[nodiscard]] bool utf8() const {
        return utf8_;
    }
    /// Draws `lines` over the previous frame.
    void draw(std::vector<std::string> lines) {
        if (!live_) {
            return;
        }
        // Cursor-up cannot reach above the window, so a frame taller than
        // the window would be redrawn in the wrong place: keep the top lines.
        const unsigned rows = wk::terminal_height();
        if (rows > 1 && lines.size() >= rows) {
            lines.resize(rows - 1);
        }
        wk::write_terminal(wk::redraw(frame_rows(), lines));
        frame_ = std::move(lines);
    }
    /// Erases the frame and leaves the cursor on its first line, so later output
    /// starts there.
    void suspend() {
        if (live_ && !frame_.empty()) {
            wk::write_terminal(wk::erase(frame_rows()));
            frame_.clear();
        }
    }
    /// Draws `lines` once more and leaves them, with the cursor below them.
    void close(std::vector<std::string> lines) {
        draw(std::move(lines));
        live_ = false;
        frame_.clear();
    }

private:
    /// Rows the frame takes on the screen at the window's current width, since
    /// long lines wrap.
    [[nodiscard]] std::size_t frame_rows() const {
        return wk::screen_rows(frame_, wk::terminal_width());
    }
    bool live_ = false;
    bool utf8_ = false;
    std::vector<std::string> frame_; ///< the lines on the screen now
};

/// The frontier as the last GET /api/stats gave it; written by the fetch
/// thread, read by the display.
struct FrontierView {
    std::mutex m;
    bool have = false;
    double log2 = 0.0;
    std::string value;    ///< decimal
    Clock::time_point at; ///< when `log2` was fetched
    bool failed = false;  ///< the last fetch failed
    std::atomic<bool> busy{false};
};

} // namespace collatz::cli::worker_detail

#endif // COLLATZ_CLI_CMD_WORKER_DISPLAY_HPP
