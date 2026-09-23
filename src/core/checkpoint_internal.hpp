#ifndef COLLATZ_CORE_CHECKPOINT_INTERNAL_HPP
#define COLLATZ_CORE_CHECKPOINT_INTERNAL_HPP

// Private declarations shared by the src/core/checkpoint*.cpp pieces: the JSON
// string escape, the file and host-name helpers, the slice field readers and
// the slice digest check, the slice-set checks of merge, and the directory and
// resume steps of run.

#include "collatz/checkpoint.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace collatz::checkpoint::detail {

// Defined in checkpoint_result_json.cpp.
std::string json_escape(std::string_view s);

// The largest file read_file loads. A slice holds at most verify::kRecordsCap
// records of up to about 100 bytes each (a 29-digit n and a peak of up to 60
// digits), about 100 MB, so a 64 MiB cap would refuse a real slice.
inline constexpr std::uintmax_t kMaxReadBytes = std::uintmax_t{256} << 20;

// Defined in checkpoint_io.cpp.
std::string host_name();
// Whether `path` is a regular file (after symlinks) of at most `max_bytes`; else
// false with the reason in `err`. A device, FIFO or socket is refused before it
// is opened, so a device or a FIFO without a writer cannot hang a read.
bool readable_file(const std::filesystem::path& path, std::uintmax_t max_bytes, std::string& err);
// Reads the whole file after readable_file, at most `max_bytes` even if it grows.
bool read_file(const std::string& path,
               std::string& out,
               std::string& err,
               std::uintmax_t max_bytes = kMaxReadBytes);
bool atomic_write(const std::filesystem::path& path, const std::string& content, std::string& err);

// Defined in checkpoint_slice.cpp.
bool get_u64(const Json* obj, const char* key, std::uint64_t& out, std::string& err);
bool get_bool(const Json* obj, const char* key, bool& out, std::string& err);

// Defined in checkpoint_slice_result.cpp: the result and preflight blocks of a
// slice file.
bool read_result(const Json* res, const Json* pre, verify::Result& r, std::string& err);

// Defined in checkpoint_digest.cpp: result.result_digest_schema is
// kResultDigestSchema and result.result_digest equals result_digest of the
// slice's opts, unwalked_depth and r.
bool check_slice_digest(const Json* res, const Slice& s, std::string& err);

// Defined in checkpoint_merge_checks.cpp.
bool check_slice_set(const std::vector<Slice>& slices, std::uint64_t K, std::string& err);
bool check_slices_match(const std::vector<Slice>& slices, std::string& err);
// The run parameters on which `a` and `b` differ, one "key: a vs b" line each:
// N, block_h, A, B, bitvectors, mod3t, partial_oee, the window terms, the
// record threshold, the resolved assumption source, unwalked_depth and the
// commit. Empty for two slices of one run. merge() and the resume scan both
// use it.
std::vector<std::string> run_params_diff(const Slice& a, const Slice& b);

// Defined in checkpoint_dir.cpp.
bool pause_file_present(const std::filesystem::path& dir);
bool prepare_dir(const RunConfig& cfg, const std::filesystem::path& dir, std::string& err);
// False, with the reason in `err`, when a slice path holds something this
// program cannot have written (not a regular file, or above kMaxReadBytes).
bool scan_slices(const RunConfig& cfg,
                 const std::filesystem::path& dir,
                 std::vector<Slice>& done,
                 std::vector<std::uint64_t>& todo,
                 std::string& err);

// Defined in checkpoint_resume.cpp.
bool adopt_run_json(RunConfig& cfg, const std::filesystem::path& run_path, std::string& err);
void init_new_run(RunConfig& cfg);
bool check_slicing(const RunConfig& cfg, std::string& err);

} // namespace collatz::checkpoint::detail

#endif // COLLATZ_CORE_CHECKPOINT_INTERNAL_HPP
