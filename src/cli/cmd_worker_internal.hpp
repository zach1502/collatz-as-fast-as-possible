#ifndef COLLATZ_CLI_CMD_WORKER_INTERNAL_HPP
#define COLLATZ_CLI_CMD_WORKER_INTERNAL_HPP

// Private declarations shared by the src/cli/cmd_worker*.cpp pieces of
// `collatz worker`: the Worker class, the server's Reply, the signal state
// and the file and JSON helpers. cmd_worker.cpp describes the worker.

#include "cmd_worker_display.hpp"
#include "cmd_worker_settings.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/progress.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef COLLATZ_GIT_COMMIT
#define COLLATZ_GIT_COMMIT "unknown"
#endif

namespace collatz::cli::worker_detail {

using collatz::checkpoint::Json;

// Counts Ctrl-C and SIGTERM. The first asks for a pause; a second one while no
// child runs exits at once (the child gets its own Ctrl-C from the console).
// While an audit child runs, the thread that waits for it terminates it and
// exits instead (start_audit), so that a quit also ends the audit.
inline std::atomic<int> g_stop{0};
inline std::atomic<bool> g_child_running{false};
inline std::atomic<bool> g_audit_running{false};
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "written from a signal handler");

// Defined in cmd_worker.cpp.
bool parse_u64(std::string_view s, std::uint64_t& out);
bool read_file(const fs::path& p, std::string& out);
void append_log(const fs::path& path, const std::string& line);
std::string tail_lines(const std::string& text, std::size_t n);
std::string num_of(const Json* obj, const char* key);

struct Reply {
    int status = 0; ///< 0 = no HTTP response
    std::optional<Json> json;
    std::string error; ///< network error, or the server's "error" message
};

/// What a failed request says: the server's "error" text (or the network
/// error), else, for a JSON reply without one, its status and a short excerpt
/// of its body. Defined in cmd_worker.cpp.
std::string reply_error(const Reply& r);

/// Whether verify can run the block of `l`. False, with the reason, for a
/// lease that verify --block would refuse or misread: H = 0 (verify takes it
/// as a full run of [0, 2^K)), a block start below 2^17 or an end above 2^96,
/// K above 76 + A, or A above the Step-2 limit. Defined in cmd_worker_lease.cpp.
bool lease_runnable(const wk::Lease& l, std::string& why);

class Worker {
public:
    // Every file the worker deletes or changes is under state_dir_, which it
    // made itself (wk::open_state_dir); the config file and worker.log stay
    // next to it.
    explicit Worker(Settings s)
        : s_(std::move(s)), dir_(s_.config_path.parent_path()),
          state_dir_(wk::state_dir(s_.config_path)) {
        tmp_dir_ = state_dir_ / "tmp";
        pause_file_ = state_dir_ / "PAUSE";
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() {
        if (frontier_thread_.joinable()) {
            frontier_thread_.join();
        }
        if (audit_thread_.joinable()) {
            // run() stops or waits for the audit before it returns, so only an
            // exception gets here; the thread holds nothing but its own state.
            audit_thread_.detach();
        }
        wk::restore_terminal();
    }

    int run();
    int print_status();

private:
    /// Renew: the lease is not live on the server (it expired or was released),
    /// so the unit is asked for again before its result is resent.
    /// TokenRefused: the server refused the worker token; the unit stays for
    /// the next run. Unsent: with --once, the result could not be submitted in
    /// kOnceTries tries; it stays for the next run.
    enum class Unit { Done, Rejected, Paused, Aborted, Failed, Renew, TokenRefused, Unsent };
    /// Lease: `out` is set. Wait: nothing now (no work, rate limit or network
    /// error; backoff_ holds the delay). Fatal: the server refused this client,
    /// or --server has no /api/fetch.
    enum class Got { Lease, Wait, Fatal };

    Reply call(const std::string& method, const std::string& path, const std::string& body);
    std::string auth_fields() const;
    bool stop_requested() const;
    /// Sleeps up to `ms`, returning early (false) when a pause is requested.
    /// The display shows `status` and the time left meanwhile.
    bool nap(unsigned ms, const std::string& status = "waiting");
    /// Next backoff delay; doubles up to 15 minutes, reset by a success.
    unsigned next_backoff();
    /// Loads the config and gives it a host id.
    bool load_identity(std::string& err);
    std::optional<fs::path> pending_unit() const;
    std::optional<wk::Lease> load_lease(const fs::path& udir, bool& released) const;
    bool save_lease(const fs::path& udir, const Json& lease, bool released) const;
    Got fetch(wk::Lease& out, fs::path& udir, std::optional<std::uint64_t> prefer);
    Unit work(const wk::Lease& l, const fs::path& udir);
    Unit submit(const wk::Lease& l, const fs::path& udir, double seconds);
    void release(const wk::Lease& l, const fs::path& udir);
    /// Marks the unit's saved lease as handed back, so the next pass asks for
    /// it with prefer_unit; the slices and result.json stay.
    bool mark_released(const fs::path& udir) const;
    /// A verify started by an earlier session (one that was killed while its
    /// child ran) may still work in udir; waits until it exits so that no
    /// second verify runs on the same checkpoint directory. False when a pause
    /// was requested meanwhile.
    bool wait_for_earlier_verify(const fs::path& udir);
    void drop(const fs::path& udir) const;
    /// The fetch-work-submit loop of run().
    int run_units();

    /// A random certify audit of an accepted unit; one is queued or running at
    /// a time.
    struct AuditJob {
        std::uint64_t unit_id = 0;
        std::string seed; ///< the unit's result_digest
        fs::path dir;     ///< audits/<unit id>: result.json, audit.json, audit.log
        std::vector<std::string> argv;
    };
    /// Shared with the thread that waits for the audit process.
    struct AuditRun {
        std::atomic<int> phase{0};     ///< 0 starting, 1 running, 2 ended
        std::atomic<bool> stop{false}; ///< set by stop_audit: terminate the process
        int rc = 0;                    ///< exit code; valid once phase is 2
        std::string err;
    };
    /// Queues an audit of the accepted unit `l` when audit_selected picks its
    /// digest, with a copy of its result.json (udir is dropped afterwards).
    void queue_audit(const wk::Lease& l, const fs::path& udir, const std::string& digest);
    /// Starts the queued audit, if any and none runs. Called right after the
    /// next verify started (so no other child is being created meanwhile), when
    /// the worker has no unit to run, and before a normal exit.
    void start_audit();
    /// Reports an audit that ended; with `wait`, first waits for a running one
    /// (a pause request meanwhile stops it).
    void finish_audit(bool wait);
    /// Drops a queued audit and terminates a running one; logged with `why`.
    /// An audit that already ended is reported as usual.
    void stop_audit(const std::string& why);
    /// Removes the audit directories that earlier sessions left (audits that
    /// never finished), except those of audits with failures or with an
    /// outcome the server has not taken.
    void remove_stale_audits();
    /// Prints, logs and posts to /api/audit the outcome of `job`. When the
    /// server cannot be reached (or answers 5xx, 401, 403, 408 or 429) the
    /// outcome is kept in the audit directory as wk::kUnreportedAudit.
    void report_audit(const AuditJob& job, const AuditRun& run);
    /// Sends the outcomes that earlier sessions kept (report_audit) again; the
    /// server answers a repeated audit as a duplicate.
    void resend_audit_reports();

    /// The block in progress, as the display shows it.
    struct UnitView {
        bool active = false;
        std::uint64_t id = 0;
        std::string h;
        unsigned K = 0;
        bool gpu = false;
        Clock::time_point t0;
        std::string started; ///< local time, "09:12"
        fs::path progress_file;
        fs::path log_file;
        bool have = false; ///< `snap` holds a line read from progress_file
        collatz::progress::Snapshot snap;
        double shown = 0.0;   ///< the largest fraction shown, which the bar keeps
        bool scan_log = true; ///< still looking for the "gpu device:" line
        Clock::time_point next_plain;
    };
    /// Opens worker.log and the terminal and draws the first frame.
    void start_display();
    /// Leaves the final frame (live) and logs why the worker stops.
    void finish_display(int rc);
    /// Redraws the display at most once a second (and prints the plain
    /// progress line once a minute); called while the worker waits.
    void tick();
    /// Ends a wait: erases the frame and waits for a frontier fetch in flight,
    /// so that no other request runs alongside the next one (on Windows two
    /// children started at once can inherit each other's request files).
    void hide_display();
    /// Reads the unit's progress file and, until found, the GPU's name from
    /// its verify log.
    void refresh_unit();
    /// Starts GET /api/stats on its own thread when the last one is 5 minutes
    /// old and finished.
    void maybe_fetch_frontier(Clock::time_point now);
    [[nodiscard]] wk::DisplayState display_state(Clock::time_point now);
    [[nodiscard]] std::string hint_text() const;
    /// Appends `line` with the local time to worker.log once the display started.
    void log_line(const std::string& line) const;

    Settings s_;
    fs::path dir_;       ///< the config file's directory: worker.json, worker.log
    fs::path state_dir_; ///< <dir_>/collatz-worker: tmp, units, audits, lock, PAUSE
    fs::path tmp_dir_;
    fs::path pause_file_;
    fs::path units_dir_;
    wk::Config cfg_;
    unsigned backoff_ = 0;
    std::string wait_reason_;
    bool no_work_ = false; ///< the last Wait was "no unit needs you", not an error

    bool display_on_ = false; ///< start_display ran (and finish_display not yet)
    Screen screen_;
    bool msys_pty_ = false;
    fs::path log_path_;
    Clock::time_point t0_;
    Clock::time_point next_tick_;
    std::string session_since_;
    std::uint64_t session_blocks_ = 0;
    double session_numbers_ = 0.0;   ///< numbers of the units submitted in this session
    double pending_numbers_ = 0.0;   ///< this session's numbers of the unit being submitted
    std::uint64_t pending_unit_ = 0; ///< the unit pending_numbers_ belongs to
    std::string last_;               ///< the last submitted unit, for the display
    std::string gpu_name_;           ///< from the last verify log that named it
    std::string status_;             ///< what the worker waits for, for the display
    bool status_countdown_ = false;
    Clock::time_point status_until_;
    std::string final_hint_;
    UnitView unit_;
    std::shared_ptr<FrontierView> frontier_ = std::make_shared<FrontierView>();
    std::thread frontier_thread_;
    Clock::time_point frontier_next_;
    std::optional<AuditJob> audit_;       ///< queued, or running while audit_run_ is set
    std::shared_ptr<AuditRun> audit_run_; ///< set from start to report
    std::thread audit_thread_;
};

} // namespace collatz::cli::worker_detail

#endif // COLLATZ_CLI_CMD_WORKER_INTERNAL_HPP
