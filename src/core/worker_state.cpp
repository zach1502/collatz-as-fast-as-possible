// The worker's state directory (state_dir, open_state_dir): the one directory
// in which the worker deletes or changes files. The worker makes it itself and
// marks it with the file kStateMarker; it takes over an empty directory and
// refuses any other.

#include "collatz/worker.hpp"
#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <string>
#include <system_error>

namespace collatz::worker {

namespace fs = std::filesystem;
using namespace detail;

namespace {

constexpr std::string_view kMarkerSchema = "collatz-worker-state/";

// A marker is a regular file, not a symlink, that starts with the schema.
bool has_marker(const fs::path& dir) {
    const fs::path marker = dir / kStateMarker;
    std::error_code ec;
    if (fs::symlink_status(marker, ec).type() != fs::file_type::regular) {
        return false;
    }
    std::string text;
    return read_text(marker, text) && text.rfind(kMarkerSchema, 0) == 0;
}

std::string refusal(const fs::path& dir, const std::string& why) {
    return dir.string() + " " + why +
           "; the worker keeps its state there and deletes files in it, so it will not use "
           "it (move it away, or give the config file another directory with --config FILE)";
}

} // namespace

fs::path state_dir(const fs::path& config_path) {
    return config_path.parent_path() / "collatz-worker";
}

bool open_state_dir(const fs::path& dir, std::string& err) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(dir, ec);
    if (st.type() == fs::file_type::symlink) {
        err = refusal(dir, "is a symlink");
        return false;
    }
    if (fs::exists(st) && st.type() != fs::file_type::directory) {
        err = refusal(dir, "is not a directory");
        return false;
    }
    if (!fs::exists(st) && !make_private_dirs(dir, err)) {
        return false;
    }
    if (has_marker(dir)) {
        return true;
    }
    if (!fs::is_empty(dir, ec) || ec) {
        err = refusal(dir,
                      std::string("was not made by collatz worker (it has no ") + kStateMarker +
                          " file)");
        return false;
    }
    std::string werr;
    // has_marker again: a worker that started at the same moment may have
    // written the marker first, which makes this exclusive create fail.
    if (create_private_file(dir / kStateMarker, std::string(kMarkerSchema) + "1\n", true, werr) ||
        has_marker(dir)) {
        return true;
    }
    err = werr;
    return false;
}

} // namespace collatz::worker
