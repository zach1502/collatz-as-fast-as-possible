#pragma once

// Host-pipeline helpers for the two-deep --gpu path: the candidate-id cap, the
// flag-list prefix read, the batch cap, the producer's candidate bound and the
// device-memory budget. They are pure functions, so the sizing rules are
// unit-tested without a device (tests/gpu/test_host_pipeline.cpp).
//
// The pipeline changes only when host work or a transfer happens, or which code
// path a batch half takes. The folds do not depend on the schedule, so every
// Result scalar is the same.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace collatz::gpu {

/// A positive integer env knob; unset, empty, unparseable or 0 gives `dflt`.
[[nodiscard]] inline std::size_t env_size_or(const char* name, std::size_t dflt) noexcept {
    const char* e = std::getenv(name);
    if (e == nullptr || e[0] == '\0') {
        return dflt;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(e, &end, 10);
    return (end == e || v == 0ULL) ? dflt : static_cast<std::size_t>(v);
}

/// The 2^29-id cap (2 GiB of 4-byte ids) can exceed CL_DEVICE_MAX_MEM_ALLOC_SIZE
/// (2,147,352,576 bytes on an RTX 3070). Returns the largest id count <= hard_cap
/// whose largest per-id buffer (`bytes_per_id`) fits one allocation, rounded
/// down to `grain` ids and never below one grain. max_alloc == 0 means unknown
/// and gives hard_cap.
[[nodiscard]] constexpr std::size_t cand_alloc_cap(std::uint64_t max_alloc,
                                                   std::uint64_t bytes_per_id,
                                                   std::size_t hard_cap,
                                                   std::size_t grain) noexcept {
    if (max_alloc == 0 || bytes_per_id == 0) {
        return hard_cap;
    }
    if (grain == 0) {
        grain = 1;
    }
    std::uint64_t ids = max_alloc / bytes_per_id;
    ids -= ids % grain;
    if (ids == 0) {
        ids = grain;
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(ids, hard_cap));
}

/// How many flag-list entries the non-blocking slot readback carries: K, but at
/// most the slot's flag-list capacity, so the read stays inside the buffer.
[[nodiscard]] constexpr std::size_t flag_prefix_len(std::size_t k, std::size_t flag_cap) noexcept {
    return std::min(k, flag_cap);
}

/// Entries the host must still read after the prefix landed (0 = none).
[[nodiscard]] constexpr std::size_t flag_rest_len(std::size_t prefix,
                                                  std::size_t flag_count) noexcept {
    return flag_count > prefix ? flag_count - prefix : 0;
}

/// Flag-list entries the in-flight slot readback carries (the prefix length).
inline constexpr std::size_t kFlagReadK = 1024;

/// The device mask packs a candidate as (s << A) | a in one 32-bit id, with s
/// the survivor's index in its batch and a < 2^A, and the tail and the host
/// decode s = id >> A, so a batch must hold at most 2^(32-A) survivors.
[[nodiscard]] constexpr std::size_t packed_id_survivors(unsigned a_bits) noexcept {
    return std::size_t{1} << (32u - a_bits);
}

/// The largest batch a run at A builds: flush_cap, but at most
/// packed_id_survivors(A). That is 2^20 at the default A = 12 and flush cap,
/// 2^19 at A = 13 and 2^18 at A = 14.
[[nodiscard]] constexpr std::size_t batch_survivor_cap(unsigned a_bits,
                                                       std::size_t flush_cap) noexcept {
    return std::min(flush_cap, packed_id_survivors(a_bits));
}

/// The candidate upper bound sum(min(a*_s, 2^A)) over a batch's a* list, which
/// cpi_submit would compute on the dispatch thread and the mask self-check
/// compares against. The producer computes it instead.
template <typename It>
[[nodiscard]] std::uint64_t cand_upper_bound(It first, It last, std::uint64_t two_a) noexcept {
    std::uint64_t s = 0;
    for (; first != last; ++first) {
        const std::uint64_t a = *first;
        s += a < two_a ? a : two_a;
    }
    return s;
}

// Device-memory budget. At B=32 the bitvectors take 4 GiB (8 levels x 512 MiB)
// and the candidate-id buffer up to 2 GiB (2^29 ids x 4 bytes), most of an
// 8 GiB card, so the estimate counts every large buffer:
//   * the bitvectors (exact),
//   * the id cap x bytes per id (4),
//   * per-survivor buffers x the largest batch, at 256 bytes per survivor (the
//     main input/output set takes 126 and the copy-queue input set 65),
//   * a fixed 256 MiB for tables, flag lists, per-group partials and slots,
//   * driver/display headroom max(256 MiB, global/16), since WDDM keeps part of
//     VRAM (512 MiB on an 8 GiB card).
// If the total does not fit CL_DEVICE_GLOBAL_MEM_SIZE, the id cap is lowered
// and the candidate-overflow split takes any batch above it. A cap >= 2^A
// always terminates, because one survivor has at most 2^A candidates. The cap
// changes only how often a batch splits, never a Result.
inline constexpr std::uint64_t kBudgetPerSurvivorBytes = 256;
inline constexpr std::uint64_t kBudgetFixedBytes = std::uint64_t{256} << 20;

/// Driver/display headroom kept free of the runner's buffers: max(256 MiB, global/16).
[[nodiscard]] constexpr std::uint64_t device_headroom(std::uint64_t global_mem) noexcept {
    const std::uint64_t floor_b = std::uint64_t{256} << 20;
    const std::uint64_t frac = global_mem / 16u;
    return frac > floor_b ? frac : floor_b;
}

struct CandCapPlan {
    bool fits = false;        ///< false => even min_cap ids do not fit (CPU fallback)
    bool lowered = false;     ///< cap < cur_cap (printed by verify_gpu)
    std::size_t cap = 0;      ///< id cap to use (== cur_cap unless lowered)
    std::uint64_t budget = 0; ///< global_mem - headroom
    std::uint64_t need = 0;   ///< estimated bytes at `cap`
};

/// Plan the candidate-id cap against the device-memory budget. `cur_cap` is
/// the cap the runner would use (the max_alloc clamp and env overrides). The
/// lowered cap is rounded down to `grain` and never below `min_cap`.
[[nodiscard]] constexpr CandCapPlan plan_cand_cap(std::uint64_t global_mem,
                                                  std::uint64_t bv_bytes,
                                                  std::uint64_t max_survivors,
                                                  std::uint64_t bytes_per_id,
                                                  std::size_t cur_cap,
                                                  std::size_t min_cap,
                                                  std::size_t grain) noexcept {
    CandCapPlan p;
    if (grain == 0) {
        grain = 1;
    }
    if (bytes_per_id == 0) {
        bytes_per_id = 1;
    }
    const std::uint64_t head = device_headroom(global_mem);
    p.budget = global_mem > head ? global_mem - head : 0;
    const std::uint64_t batch_other = max_survivors * kBudgetPerSurvivorBytes + kBudgetFixedBytes;
    const auto need_at = [&](std::uint64_t cap) {
        return bv_bytes + batch_other + cap * bytes_per_id;
    };
    p.cap = cur_cap;
    p.need = need_at(cur_cap);
    if (p.need <= p.budget) {
        p.fits = true;
        return p;
    }
    // Lower the cap: room left for ids after the bitvectors + other buffers.
    // On failure `need` reports the requirement at the floor `min_cap`.
    if (p.budget <= bv_bytes + batch_other) {
        p.need = need_at(min_cap);
        return p; // fits == false
    }
    std::uint64_t ids = (p.budget - bv_bytes - batch_other) / bytes_per_id;
    ids -= ids % grain;
    if (ids < min_cap || ids == 0) {
        p.need = need_at(min_cap);
        return p; // fits == false
    }
    p.cap = static_cast<std::size_t>(ids < cur_cap ? ids : cur_cap);
    p.lowered = p.cap < cur_cap;
    p.need = need_at(p.cap);
    p.fits = p.need <= p.budget;
    return p;
}

} // namespace collatz::gpu
