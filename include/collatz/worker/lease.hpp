#ifndef COLLATZ_WORKER_LEASE_HPP
#define COLLATZ_WORKER_LEASE_HPP

// The lease a unit is handed out with, the verify and certify command lines for
// it, the fields that name it in a request, the files a unit keeps for it, and
// the choice of the units that are audited.

#include "collatz/checkpoint.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace collatz::worker {

/// One unit as handed out by POST /api/fetch. parse_lease checks every field
/// that reaches the verify command line (decimal H, bounded integers, the
/// kernel table16, a plain-text assumption source), so a server cannot inject
/// options.
struct Lease {
    std::uint64_t unit_id = 0;
    /// The lease's own id, which submit and release send back (lease_fields),
    /// so that a result counts only on the lease it was made on. A renewed
    /// lease keeps it. 0 when the server gives none (a server from before
    /// lease ids).
    std::uint64_t lease_id = 0;
    std::uint64_t campaign_id = 0;
    std::string h;  ///< decimal block index H: the unit is [H*2^K, (H+1)*2^K)
    unsigned K = 0; ///< the block's size, chosen by the server (at most the asked --block-bits)
    unsigned A = 0;
    unsigned B = 0;
    unsigned bitvectors = 0;
    unsigned mod3t = 0;
    unsigned window_chains = 0;
    unsigned window_templates_d = 0;
    bool partial_oee = true;
    std::string kernel;
    std::string record_threshold;   ///< decimal, or "none"
    std::string assumption_source;  ///< empty: the binary's default
    std::int64_t lease_expires = 0; ///< Unix seconds
    /// "gpu" or "cpu": the backend the server asks the unit to run on; empty
    /// when the lease does not say. Only moves a --gpu worker's unit to the
    /// CPU (lease_uses_gpu).
    std::string backend;
    /// The server's remark on the block (for example that it is smaller than
    /// asked for); empty when none. Printed through printable().
    std::string note;
};
[[nodiscard]] bool parse_lease(const checkpoint::Json& lease, Lease& out, std::string& err);

/// Whether the unit of `l` runs on the GPU: only on a worker started with
/// --gpu (`worker_gpu`), and not when the lease asks for the CPU. A lease
/// never turns the GPU on for a worker started without --gpu.
[[nodiscard]] bool lease_uses_gpu(const Lease& l, bool worker_gpu) noexcept;

/// The members of a submit or release body that name the lease of `l`:
/// `"unit_id":N`, then `,"lease_id":M` when the server gave the lease an id.
[[nodiscard]] std::string lease_fields(const Lease& l);

/// Keeps the slices (`ck/`) and result.json of unit directory `udir` only for
/// the lease they were made on, whose id the file `udir/lease-id` holds: when
/// it names another lease, or is missing, both are removed, so a unit that
/// was started over or handed out afresh runs from scratch rather than
/// sending an earlier result again. The file then holds the id of `l`.
void keep_unit_files(const std::filesystem::path& udir, const Lease& l);

/// Arguments after the program name for `collatz verify` on this unit.
/// threads 0 = all hardware threads. A non-empty `progress_path` adds
/// --progress-file (collatz/progress.hpp), which changes no result.
[[nodiscard]] std::vector<std::string> verify_args(const Lease& l,
                                                   unsigned threads,
                                                   bool gpu,
                                                   const std::string& checkpoint_dir,
                                                   const std::string& json_path,
                                                   const std::string& progress_path = {});

/// Arguments after the program name for the random certify audit of a unit
/// (`collatz certify --block ... --audit-seed SEED`):
/// the lease's Step-1 and Step-2 parameters, `threads` CPU threads, the unit's
/// `verify --json` file for the peak and record checks, and the file the audit
/// writes its JSON to. With --audit-cache-only: the audit uses the tables the
/// unit's verify cached and fails rather than build them.
[[nodiscard]] std::vector<std::string> audit_args(const Lease& l,
                                                  const std::string& seed,
                                                  unsigned threads,
                                                  const std::string& result_path,
                                                  const std::string& json_path);

/// Whether the unit with result digest `digest` is audited at rate `rate`
/// (clamped to [0, 1]): the first 8 bytes of SHA-256("collatz-audit-select|"
/// + digest), big-endian, are below rate * 2^64. The digest is unknown before
/// the unit is done, and the choice can be recomputed from it afterwards.
[[nodiscard]] bool audit_selected(std::string_view digest, double rate) noexcept;

} // namespace collatz::worker

#endif // COLLATZ_WORKER_LEASE_HPP
