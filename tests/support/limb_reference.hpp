#pragma once

// Helpers shared by the files of test_gpu_u128_limbs (tests/gpu/test_u128_limbs.cpp
// and test_u128_limbs/): conversion between u128q and collatz::u128, a random
// u128, the packed 16-step table, and u128q_tail16 (the u64 + limb loop on the
// host) as a TailOut.
#include "collatz/collatz_fn.hpp"
#include "collatz/gpu/u128_limbs.h"
#include "collatz/u128.hpp"

#include <array>
#include <cstdint>
#include <random>

namespace collatz::test::limbs {

[[nodiscard]] inline u128 to_native(u128q q) {
    return collatz::make_u128(u128q_hi64(q), u128q_lo64(q));
}

[[nodiscard]] inline u128q from_native(u128 x) {
    return u128q_from_halves(collatz::lo64(x), collatz::hi64(x));
}

// A random u128 with a random bit width in [0, 128], covering small values,
// limb boundaries, and near-max values.
[[nodiscard]] inline u128 rand_u128(std::mt19937_64& rng) {
    const int bits = static_cast<int>(rng() % 129);
    if (bits == 0) {
        return 0;
    }
    u128 x = collatz::make_u128(rng(), rng());
    if (bits < 128) {
        x &= (static_cast<u128>(1) << bits) - 1;
    }
    return x;
}

[[nodiscard]] inline std::array<std::uint32_t, 1u << 16> build_tbl16_local() {
    std::array<std::uint32_t, 1u << 16> tbl{};
    for (std::size_t r = 0; r < tbl.size(); ++r) {
        u128 v = static_cast<u128>(r);
        unsigned f = 0;
        for (unsigned j = 0; j < 16; ++j) {
            if ((v & 1) != 0) {
                ++f;
            }
            v = collatz::T(v).value;
        }
        tbl[r] = (static_cast<std::uint32_t>(v) << 5) | static_cast<std::uint32_t>(f);
    }
    return tbl;
}

struct TailOut {
    u128q_u32 blocks;
    u128 max_v;
    u128q_u32 ovf;
    u128 v_end;
};

[[nodiscard]] inline TailOut run_old(u128 n, u128 v, const std::uint32_t* tbl) {
    u128q mx;
    u128q ve;
    u128q_u32 ovf = 0;
    const u128q_u32 b = u128q_tail16(from_native(n), from_native(v), tbl, &mx, &ovf, &ve);
    return {b, to_native(mx), ovf, to_native(ve)};
}

} // namespace collatz::test::limbs
