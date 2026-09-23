#pragma once

// Private declarations shared by the Step 3 sources: the mpz to u128 conversion of
// the GMP replays, the static table holder, and the 16-step block constants, table
// and per-block helpers.

#include "collatz/collatz_fn.hpp"
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gmp.h>
#include <gmpxx.h>

namespace collatz::step3::detail {

/// Converts an exact mpz to u128, saturating to u128_max if it does not fit.
[[nodiscard]] static inline u128 mpz_to_u128_saturating(const mpz_class& m) {
    if (mpz_sizeinbase(m.get_mpz_t(), 2) > 128) {
        return u128_max;
    }
    u128 out = 0;
    if (from_string(oracle::to_decimal(m), out)) {
        return out;
    }
    return u128_max;
}

/// A fixed-size lookup table (2^K residues, size known at compile time) held in
/// static storage and built once, in place, by `fill` on first use. No heap
/// allocation and no multi-MB stack temporary (the fill writes straight into
/// the static object, which matters on 512 KB worker-thread stacks), and the
/// table address is a link-time constant. It is not constinit: 2^16 x 16
/// T-steps exceeds clang's default -fconstexpr-steps budget and would add
/// 256 KB to the binary.
template <class T, std::size_t N>
struct StaticTable {
    std::array<T, N> t{};
    template <class Fill>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload): StaticTable is never copied
    explicit StaticTable(Fill&& fill) {
        fill(t);
    }
};

/// Packed 16-step table: TBL16[r] = (T^16(r) << 5) | f, where f is the odd-step
/// count of r's 16-step block and c = T^16(r). For r < 2^16 the block value
/// c = T^16(r) <= 65535 * (3/2)^16 < 2^26, so it fits in 27 bits and f (<= 16)
/// in the low 5 bits: the whole entry is a single uint32. 2^16 entries = 256 KB
/// (fits L2), the compact dependent-chain state the block loop walks.
constexpr unsigned kBlockStep = 16;
constexpr std::uint32_t kFMask = 0x1Fu;                 // low 5 bits hold f (<= 16)
constexpr u128 kPow3Step = static_cast<u128>(43046721); // 3^16
constexpr u128 kPow3StepBits = 26;                      // bit_length(3^16) = 26

/// The 2^16-entry block table (see StaticTable): the residue count is a
/// compile-time constant, so the table is a std::array in static storage.
constexpr std::size_t kBlockEntries = static_cast<std::size_t>(1) << kBlockStep;
using BlockTable16 = std::array<std::uint32_t, kBlockEntries>;

/// 3^f for f in [0, 16] as 32-bit values (3^16 = 43046721 < 2^26 fits u32). The
/// block table only ever stores f <= 16, so the block landing multiply
/// `(v >> 16) * 3^f` has a multiplier < 2^26. A u32 multiplier lets the plain
/// multiply below drop the high-limb cross-products (b_hi == 0), which costs
/// less than the full __builtin_mul_overflow of the general checked_mul.
constexpr std::array<std::uint32_t, kBlockStep + 1> kPow3Low = [] {
    std::array<std::uint32_t, kBlockStep + 1> t{};
    std::uint32_t v = 1;
    for (std::size_t i = 0; i <= kBlockStep; ++i) {
        t[i] = v;
        v *= 3u;
    }
    return t;
}();

/// u64 fast-lane bar. A block advance `next = (v >> 16) * 3^f + c` is exact in
/// u64 whenever `v < 2^54`: then `v >> 16 < 2^38` and the largest multiplier is
/// `3^16 = 43046721 < 2^26`, so the product is < 2^63.4, with room for `+ c`
/// (c < 2^27). Most tail blocks sit below this bar (values descend after the
/// trajectory peak), so a block costs one 64x32 `madd` instead of a u128
/// multiply. A block that rises back above 2^54 takes the u128 path in the next
/// iteration (v is kept full-width), so the result is bit-identical for any N.
constexpr u128 kU64FastBar = static_cast<u128>(1) << 54u;

/// Advances one 16-step block: `next = (v >> 16) * 3^f + c`, the table landing
/// (f <= 16, c < 2^27). Returns false iff the true product exceeds u128; the
/// caller then falls back to the GMP replay.
///
/// Fast path: when `hi64(v >> 16) < 2^37` the product `(v>>16) * 3^f` is
/// < 2^(37+64) * 2^26 < 2^128 with room for `+ c`, so it cannot overflow and a
/// plain multiply by the 32-bit multiplier needs no overflow check. For N <= 64
/// every reachable `v` is far under this bar (the global max excursion is
/// < 2^78), so the guard is one high-limb compare that always passes. The slow
/// path keeps the exact overflow contract for the general u128 case. `next` is
/// bit for bit that of checked_mul + checked_add.
[[nodiscard]] static inline bool block_landing(u128 v, unsigned f, u128 c, u128& next) noexcept {
    const u128 hv = v >> kBlockStep;
    if (hi64(hv) < (static_cast<std::uint64_t>(1) << 37u)) {
        next = hv * static_cast<u128>(kPow3Low[f]) + c; // provably no overflow
        return true;
    }
    bool ovf = false;
    const u128 mult = checked_mul(pow3(f), hv, ovf);
    if (ovf) {
        return false;
    }
    next = checked_add(c, mult, ovf);
    return !ovf;
}

// Defined in step3_lanes.cpp.
[[nodiscard]] const BlockTable16& block_table16();

/// Proven upper bound on the maximum interior value of the 16-step block
/// starting at v: every T^j(v) (0 <= j <= 16) satisfies
/// T^j(v) <= floor((v + 1) * 3^16 / 2^16) (proof: T^j(v) <= (3/2)^j (v+1) - 1 by
/// induction, maximal at an all-odd run). Sets `overflow` when (v + 1) * 3^16
/// would exceed u128 (v beyond ~2^101, unreachable for N <= 64), in which case
/// the caller conservatively treats the block as a record contender.
[[nodiscard]] static inline u128 block_peak_ub(u128 v, bool& overflow) noexcept {
    overflow = false;
    if (bit_length(v) + static_cast<int>(kPow3StepBits) >= 128) {
        overflow = true;
        return u128_max;
    }
    return ((v + 1) * kPow3Step) >> kBlockStep;
}

/// Exact peak of the 16-step block starting at v, by single-stepping (rare: only
/// for record-contending blocks). Updates `maxv`. Returns false and leaves the
/// walk partial if a T-step overflows u128 (the caller falls back to the GMP
/// replay).
[[nodiscard]] static inline bool block_exact_peak(u128 v, u128& maxv) noexcept {
    u128 w = v;
    for (unsigned i = 0; i < kBlockStep; ++i) {
        const StepResult sr = collatz::T(w);
        if (sr.overflow) {
            return false;
        }
        w = sr.value;
        if (w > maxv) {
            maxv = w;
        }
    }
    return true;
}

} // namespace collatz::step3::detail
