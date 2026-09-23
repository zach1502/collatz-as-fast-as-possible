#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_BATCH_TYPES_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_BATCH_TYPES_HPP

// Inputs, outputs and constants of one SurvivorRunner dispatch (included by
// survivor_runtime.hpp).

#include "collatz/gpu/opencl.hpp"
#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace collatz::gpu {

/// Per-batch survivor inputs as a structure of arrays, indexed by survivor. The
/// host folds f into the per-survivor inv3f, bvbase, bvmode and p3f, so a batch
/// may mix values of f.
struct SurvivorBatch {
    std::vector<cl_uint4> n0;    ///< survivor class representative n0.
    std::vector<cl_uint4> m;     ///< m = T^M(n0).
    std::vector<cl_uint4> p3f;   ///< 3^f as 4 limbs.
    std::vector<cl_uint> astar;  ///< partial-OEE prefix a_star (twoA = no prune).
    std::vector<cl_uint> inv3f;  ///< inv(3^f) mod 2^B.
    std::vector<cl_uint> bvbase; ///< BV word offset for this f (0 if all-ones).
    std::vector<cl_uint> bvmode; ///< 1 = all-ones lookahead window (no bitvector).
    std::vector<cl_uint> f0;     ///< survivor odd-step count (node.f)
    /// Upload-trim hints: OR of the high 64 bits (limbs .z|.w) over every n0 and
    /// every m of the batch, filled in by the producer (build_inputs in
    /// verify_gpu); ~0 = unknown. Only a hint: the runner re-checks each value
    /// while packing and uploads n0 or m as uint4 limbs if one does not fit.
    cl_uint hint_n0_hi = ~0u;
    cl_uint hint_m_hi = ~0u;
    /// sum(min(astar[s], cupper_two_a)), computed by the producer right after
    /// it builds the batch, so cpi_submit on the dispatch thread can skip its
    /// own O(n) loop. Valid only when cupper_two_a equals the run's 2^A (0 = no
    /// hint). clear() resets it.
    std::uint64_t cupper_hint = 0;
    std::uint64_t cupper_two_a = 0;
    /// The pack-pool block holding this batch's packed upload (-1 = none) and
    /// the block generation it was packed under. The runner re-validates the
    /// reference (generation, state, n0 identity, trim bits) before uploading
    /// from the block, and otherwise packs from the vectors. clear() resets it.
    int pp_block = -1;
    std::uint64_t pp_gen = 0;

    [[nodiscard]] std::size_t size() const noexcept {
        return n0.size();
    }
    void clear() noexcept {
        n0.clear();
        m.clear();
        p3f.clear();
        astar.clear();
        inv3f.clear();
        bvbase.clear();
        bvmode.clear();
        f0.clear();
        hint_n0_hi = ~0u;
        hint_m_hi = ~0u;
        cupper_hint = 0;
        cupper_two_a = 0;
        pp_block = -1;
        pp_gen = 0;
    }
};

/// Per-item outputs of survivor_tail16.
struct SurvivorOut {
    std::vector<cl_ulong> cov;    ///< coverage partial sum mod 2^61-1.
    std::vector<cl_ulong> dig;    ///< pow digest partial sum mod 2^61-1.
    std::vector<cl_ulong> blocks; ///< wrapping u64 sum of 16-step block counts.
    std::vector<cl_uint> cand;    ///< candidates processed (after preflight skip).
    std::vector<cl_uint4> maxv;   ///< max block-start value over the item's candidates.
    std::vector<cl_uint> ovf;     ///< overflow/cap flag (unreachable for N <= 64).
    std::vector<cl_uint> pc;      ///< mask popcount (for step2_masked_out).
    std::vector<cl_ulong> chk;    ///< self-check record hash, keyed by the nonce.
    bool sc_ok = true;            ///< every record passed the self-check.
    const char* sc_why = nullptr; ///< first failed check.

    void resize(std::size_t n) {
        cov.resize(n);
        dig.resize(n);
        blocks.resize(n);
        cand.resize(n);
        maxv.resize(n);
        ovf.resize(n);
        pc.resize(n);
        chk.resize(n);
        sc_ok = true;
        sc_why = nullptr;
    }
};

/// Scalar (whole-run-constant) kernel arguments.
struct SurvivorConst {
    cl_uint b_mask = 0;     ///< 2^B - 1.
    cl_uint twoA = 0;       ///< 2^A.
    cl_uint m_shift = 0;    ///< M = N - A.
    cl_uint pow2m_mod9 = 1; ///< 2^M mod 9.
    cl_uint4 preflimit{};   ///< skip n < preflimit.
    cl_uint a_bits = 0;     ///< A, the candidate-id packing shift.
};

/// Result of one candidate-per-item (cpi) batch. The host has already reduced
/// the per-work-group partials, so verify_gpu only adds these scalars to its
/// run-wide folds. `flags` holds the packed (s << a_bits | a) ids of the
/// tail-flagged record contenders; it is empty, with `flag_overflow` set, when
/// the flag list overflowed, and the caller then falls back to the
/// per-survivor kernel. `sum_pc` is the Step-2 mask-bit total (= C).
struct CpiResult {
    std::uint64_t coverage = 0;  ///< sum mod 2^61-1 of the per-candidate fold.
    std::uint64_t digest = 0;    ///< pow digest, sum mod 2^61-1.
    std::uint64_t blocks = 0;    ///< wrapping u64 sum of 16-step block counts.
    std::uint64_t processed = 0; ///< candidates processed (after preflight skip).
    std::uint64_t sum_pc = 0;    ///< sum of Step-2 mask popcounts (= candidate count C).
    collatz::u128 max_bs = 0;    ///< max block-start value over the batch.
    bool flag_overflow = false;  ///< flag list exceeded capacity (caller falls back).
    /// The candidate-id buffer (not the flag list) overflowed, so the mask did
    /// not complete; `flag_overflow` is also set. The caller splits the batch
    /// by survivors and re-runs each half instead of falling back to the
    /// per-survivor kernel.
    bool cand_overflow = false;
    std::uint32_t flag_count = 0;     ///< number of tail-flagged candidates.
    std::vector<std::uint32_t> flags; ///< packed flagged ids (size min(flag_count, cap)).
    /// Per-batch self-check. sc_ok is false when any check failed, and the
    /// caller must not fold the result. sc_checked is true when the tail's
    /// nonce-keyed group records were verified.
    bool sc_ok = true;
    bool sc_checked = false;
    const char* sc_why = nullptr;
    std::uint32_t sc_tail_ids = 0; ///< tail's sum of u128q_sc_term over claimed ids.
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_BATCH_TYPES_HPP
