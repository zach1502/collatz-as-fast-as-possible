#include "cmd_verify_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/cli.hpp"
#include "collatz/progress.hpp"
#include "collatz/verify.hpp"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

// verify proves that every n < 2^N, or every n in one block, descends below
// itself or merges into the trajectory of a smaller number.
//
// run_verify calls the stages in order. They are defined in the other
// src/cli/cmd_verify_*.cpp files and declared in cmd_verify_internal.hpp.

namespace {

using namespace collatz::cli::verify_detail;

/// Writes the Result with its params, kernel, commit and machine to `path` as
/// JSON, rendered by collatz::checkpoint::result_json as for the --checkpoint
/// slice files. u128 values are decimal strings so that no reader loses
/// precision; the other scalars fit in u64 and are JSON numbers, which the
/// server's parser (server/src/json.ts) keeps as exact text. The `preflight`
/// block lets `collatz merge` subtract the preflight that every slice repeats.
/// Timing fields are not part of the merged result. The file is written through
/// PATH.tmp and a rename (write_file_atomic), so a kill leaves the old file or
/// the new one, never a truncated one. Returns false, with the reason in `err`,
/// on an I/O error.
bool write_json_result(const std::string& path,
                       const collatz::verify::Options& opts,
                       const collatz::verify::Result& r,
                       const char* backend,
                       bool use_gpu,
                       std::string& err) {
    collatz::checkpoint::JsonMeta meta;
    meta.commit = COLLATZ_GIT_COMMIT;
    meta.backend = backend;
    meta.use_gpu = use_gpu;
    const std::string doc = collatz::checkpoint::result_json(meta, opts, r);
    return collatz::checkpoint::write_file_atomic(path, doc, err);
}

int run_verify(int argc, char** argv) {
    collatz::verify::Options opts;
    set_cli_defaults(opts);
    VerifyArgs va;
    if (const int rc = parse_args(argc, argv, opts, va); rc != 0) {
        return rc;
    }
    if (va.help) {
        print_usage(stdout);
        return 0;
    }
    if (const int rc = check_args(opts, va); rc != 0) {
        return rc;
    }
    if (va.use_gpu) {
        print_gpu_device();
    }
    apply_auto_a(opts, va);
    apply_auto_b(opts, va);

    // Block-mode preconditions (and the record threshold), checked up front so
    // a --checkpoint run refuses before writing anything.
    try {
        collatz::verify::validate_block(opts);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }

    collatz::verify::Result r;
    std::string backend_str = "cpu";
#if !(defined(COLLATZ_HAS_OPENCL) && COLLATZ_HAS_OPENCL)
    if (va.use_gpu) {
        std::fprintf(stderr, "note: --gpu requested but built without OpenCL; using CPU\n");
    }
#endif
    // Destroyed after the run (below, or on an early return), which writes the
    // final counts once more.
    std::optional<collatz::progress::FileWriter> progress_writer;
    start_progress_writer(progress_writer, va.progress_path);
    if (!va.checkpoint_dir.empty()) {
        const std::optional<int> rc = run_checkpointed(opts, va, argc, argv, r, backend_str);
        if (rc) {
            return *rc;
        }
    } else {
        try {
            r = run_backend(opts, va.use_gpu, backend_str);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 2;
        }
    }
    progress_writer.reset();
    const char* backend = backend_str.c_str();
    print_result(opts, va, r, backend);

    if (!va.json_path.empty()) {
        std::string werr;
        if (!write_json_result(va.json_path, opts, r, backend, va.use_gpu, werr)) {
            std::fprintf(stderr,
                         "error: failed to write JSON to '%s' (%s)\n",
                         va.json_path.c_str(),
                         werr.c_str());
            return 2;
        }
    }
    return 0;
}

} // namespace

COLLATZ_REGISTER_COMMAND("verify", "end-to-end Step1->Step3 verifier", run_verify)
