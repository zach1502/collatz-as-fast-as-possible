#include "cmd_verify_internal.hpp"
#include "collatz/checkpoint.hpp" // pause/resume: --checkpoint DIR
#include "collatz/progress.hpp"   // --progress-file PATH
#include "collatz/verify.hpp"

#if defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL
#include "opencl_runtime.hpp" // selected_device_name
#include "survivor_runtime.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

// Runs a verification: the backend call, the --progress-file writer and the
// --checkpoint slice driver with its pause handling.

namespace collatz::cli::verify_detail {

namespace {

// With --checkpoint, the first SIGINT asks for a stop after the current slice
// is written, and a second one exits at once and loses the slice in progress;
// every finished slice is already on disk. std::signal with an atomic flag
// works because MinGW delivers a console Ctrl-C as SIGINT. The
// MSVCRT and UCRT runtimes reset the handler to SIG_DFL on delivery, so the
// handler installs itself again.
extern "C" void checkpoint_on_sigint(int /*sig*/) {
    std::atomic<int>& f = collatz::checkpoint::pause_flag();
    if (f.load() != 0) {
        std::_Exit(130);
    }
    f.store(1);
    std::signal(SIGINT, checkpoint_on_sigint);
}

// The command line for the resume hint, with double quotes around arguments
// that hold blanks or quotes.
std::string join_command(int argc, char** argv) {
    std::string s = "collatz";
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        s += ' ';
        if (a.find_first_of(" \t\"'") != std::string::npos) {
            s += '"' + a + '"';
        } else {
            s += a;
        }
    }
    return s;
}

collatz::checkpoint::RunConfig checkpoint_config(const collatz::verify::Options& opts,
                                                 const VerifyArgs& va,
                                                 int argc,
                                                 char** argv) {
    namespace ckp = collatz::checkpoint;
    ckp::RunConfig cfg;
    cfg.dir = va.checkpoint_dir;
    cfg.K = va.ck_slices;
    cfg.K_explicit = va.ck_slices != 0;
    cfg.lo = va.ck_lo;
    cfg.hi = va.ck_hi;
    cfg.range_explicit = va.ck_range;
    cfg.force = va.ck_force;
    cfg.opts = opts;
    cfg.meta.commit = COLLATZ_GIT_COMMIT;
    cfg.meta.backend = va.use_gpu ? "gpu" : "cpu";
    cfg.meta.use_gpu = va.use_gpu;
    cfg.fingerprint = ckp::make_fingerprint(opts, va.use_gpu, COLLATZ_GIT_COMMIT);
    cfg.command_line = join_command(argc, argv);
    return cfg;
}

} // namespace

// Prints "gpu device: NAME" for the device the GPU path picks; `collatz worker`
// reads the line from the verify log to name the GPU.
void print_gpu_device() {
#if defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL
    if (const std::optional<std::string> dev = collatz::gpu::selected_device_name()) {
        std::fprintf(stderr, "gpu device: %s\n", dev->c_str());
    }
#endif
}

// Runs the whole range, or one slice when o.step1.case_k > 1, on the selected
// backend, for the plain path and for each --checkpoint slice.
collatz::verify::Result
run_backend(const collatz::verify::Options& o, bool use_gpu, std::string& be) {
    collatz::verify::Result rr;
    if (use_gpu) {
#if defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL
        bool used_gpu = false;
        std::string gpu_msg;
        rr = collatz::gpu::verify_gpu(o, &used_gpu, &gpu_msg);
        be = used_gpu ? "gpu" : "cpu (fallback)";
        if (!gpu_msg.empty()) {
            std::fprintf(stderr, "%s\n", gpu_msg.c_str());
        }
#else
        rr = collatz::verify::run(o);
        be = "cpu (no opencl)";
#endif
    } else {
        rr = collatz::verify::run(o);
        be = "cpu";
    }
    return rr;
}

// --progress-file PATH.
void start_progress_writer(std::optional<collatz::progress::FileWriter>& writer,
                           const std::string& path) {
    if (!path.empty()) {
        writer.emplace(std::filesystem::path(path), std::chrono::seconds(2));
    }
}

// The --checkpoint run. Returns the exit code when it ends without a merged
// result (error, pause, or a finished --slice-range share), else nullopt with
// `r` and `backend_str` set.
std::optional<int> run_checkpointed(const collatz::verify::Options& opts,
                                    const VerifyArgs& va,
                                    int argc,
                                    char** argv,
                                    collatz::verify::Result& r,
                                    std::string& backend_str) {
    namespace ckp = collatz::checkpoint;
    ckp::RunConfig cfg = checkpoint_config(opts, va, argc, argv);
    const std::string& checkpoint_dir = va.checkpoint_dir;

    ckp::pause_flag().store(0);
    std::signal(SIGINT, checkpoint_on_sigint);
    // The watcher announces a pause request (Ctrl-C or DIR/PAUSE) at once; the
    // slice in progress still runs to its end and is written. It starts from
    // cfg.on_ready, after ckp::run has removed a stale DIR/PAUSE, so an old
    // file is not announced as a new request. The guard stops and joins it on
    // every exit path, exceptions included, because destroying a joinable
    // std::thread during unwinding terminates the program.
    struct WatcherGuard {
        std::atomic<bool> stop{false};
        std::thread th;
        WatcherGuard() = default;
        WatcherGuard(const WatcherGuard&) = delete;
        WatcherGuard& operator=(const WatcherGuard&) = delete;
        ~WatcherGuard() {
            stop.store(true);
            if (th.joinable()) {
                th.join();
            }
        }
    } watcher;
    cfg.on_ready = [&watcher, &checkpoint_dir] {
        watcher.th =
            std::thread([&watcher, pause_file = std::filesystem::path(checkpoint_dir) / "PAUSE"] {
                bool announced = false;
                while (!watcher.stop.load()) {
                    std::error_code wec;
                    if (!announced && (ckp::pause_flag().load() != 0 ||
                                       std::filesystem::exists(pause_file, wec))) {
                        announced = true;
                        std::fprintf(stderr,
                                     "\ncheckpoint: pause requested -- finishing the current "
                                     "slice, then stopping (Ctrl-C again aborts now; finished "
                                     "slices are kept)\n");
                        std::fflush(stderr);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            });
    };
    std::string ck_err;
    ckp::MergeInfo ck_info;
    const ckp::Outcome oc = ckp::run(
        cfg,
        [&](const collatz::verify::Options& o, std::string& be) {
            return run_backend(o, va.use_gpu, be);
        },
        r,
        ck_err,
        &ck_info);
    watcher.stop.store(true);
    if (watcher.th.joinable()) {
        watcher.th.join();
    }
    std::signal(SIGINT, SIG_DFL);
    switch (oc) {
    case ckp::Outcome::Error:
        std::fprintf(stderr, "error: %s\n", ck_err.c_str());
        return 2;
    case ckp::Outcome::Paused:
        std::fprintf(stderr,
                     "checkpoint: paused (%s). Finished slices are saved in %s.\n"
                     "  Resume later with the same command:\n    %s\n",
                     ck_err.c_str(),
                     checkpoint_dir.c_str(),
                     cfg.command_line.c_str());
        return 4;
    case ckp::Outcome::RangeDone:
        std::fprintf(stderr,
                     "checkpoint: %s (%s). When every machine has finished its range, "
                     "copy the slice directories to one place and merge them:\n"
                     "    collatz merge DIR1 DIR2 ...\n",
                     ck_err.c_str(),
                     checkpoint_dir.c_str());
        return 0;
    case ckp::Outcome::Complete:
        break;
    }
    backend_str = ck_info.backend; // every slice's backend (or "mixed: ...")
    std::fprintf(stderr,
                 "checkpoint: all slices done; merged result written to %s\n",
                 (std::filesystem::path(checkpoint_dir) / "merged.json").string().c_str());
    return std::nullopt;
}

} // namespace collatz::cli::verify_detail
