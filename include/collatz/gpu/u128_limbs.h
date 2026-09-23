/* u128_limbs.h -- 128-bit unsigned arithmetic as 4 x uint32 limbs.
 *
 * Compiled both as OpenCL C (prepended to each kernel source) and as host C++
 * (the GPU host code and the unit tests), switched on the OpenCL-only macro
 * __OPENCL_VERSION__, so the GPU kernels and the host share one copy of the
 * limb math and cannot drift apart.
 *
 * Representation: struct u128q { uint w[4]; }, little-endian, w[0] holds the
 * least-significant 32 bits.
 *
 * Host C++ builds with -Werror -Wold-style-cast and OpenCL C has no
 * static_cast, so every cast goes through U128Q_CAST: a C-style cast under
 * OpenCL, a static_cast under C++.
 */
#ifndef COLLATZ_GPU_U128_LIMBS_H
#define COLLATZ_GPU_U128_LIMBS_H
/* NOLINTBEGIN(modernize-use-nullptr): OpenCL C has no nullptr, so null pointers are 0. */

#include "collatz/gpu/u128_limbs/u128_limbs_01_types_arith.h"
#include "collatz/gpu/u128_limbs/u128_limbs_02_tail16.h"

/* q = a / d, *rem = a mod d, for a 32-bit divisor d != 0 (4-limb long
 * division; each partial (rem << 32 | limb) < d * 2^32 fits u64). */
U128Q_INLINE u128q u128q_divmod_u32(u128q a, u128q_u32 d, u128q_u32* rem) {
    u128q q;
    u128q_u64 r = 0;
    int i;
    for (i = 3; i >= 0; --i) {
        const u128q_u64 cur = (r << 32) | U128Q_CAST(u128q_u64, a.w[i]);
        q.w[i] = U128Q_CAST(u128q_u32, cur / d);
        r = cur % d;
    }
    *rem = U128Q_CAST(u128q_u32, r);
    return q;
}

/* Exact contender-flag threshold for the 16-step tail, computed
 * once per dispatch on the host. The direct rule is
 *   flag = ovf_item || ovm || floor(cmax * 3^16 / 2^16) >= r,
 * with ovm = (cmax * 3^16 >= 2^128) and the floor taken of the exact product
 * when ovm == 0. With K = 3^16 = 43046721 and integers c, r >= 0:
 *   (a) floor(c*K / 2^16) >= r  <=>  c*K >= r*2^16  (floor(x/m) >= r <=> x >= r*m)
 *                               <=>  c >= ceil(r*2^16 / K) =: T_r,
 *   (b) ovm                     <=>  c*K >= 2^128  <=>  c >= ceil(2^128 / K) =: T_ovf.
 * When ovm == 1 the direct rule flags regardless of (a), so it is
 * (b) || (not (b) && (a)) = (a) || (b) <=> c >= min(T_r, T_ovf). So
 *   flag = ovf_item || cmax >= u128q_flag_threshold16(r)
 * is the same decision for every c < 2^128, i.e. one compare replaces the
 * mul_u32 + shift + compare_ge per candidate.
 * T_r without a 144-bit product: r = q*K + rem (rem < K), so
 *   r*2^16 / K = q*2^16 + rem*2^16 / K  and  T_r = q*2^16 + ceil(rem*2^16 / K).
 * q < 2^128 / K < 2^103, so q*2^16 < 2^119 and adding ceil(..) <= 2^16 cannot
 * wrap. T_ovf = floor((2^128 - 1) / K) + 1 (K is odd, so K does not divide
 * 2^128 and ceil(2^128/K) = floor((2^128-1)/K) + 1). The unit test checks the
 * decision against the direct rule at c = T-1, T, T+1 and at random c. */
U128Q_INLINE u128q u128q_flag_threshold16(u128q r) {
    const u128q_u32 k = 43046721u; /* 3^16 */
    u128q_u32 rem = 0;
    const u128q q = u128q_divmod_u32(r, k, &rem);
    u128q tr;
    tr.w[0] = q.w[0] << 16;
    tr.w[1] = (q.w[1] << 16) | (q.w[0] >> 16);
    tr.w[2] = (q.w[2] << 16) | (q.w[1] >> 16);
    tr.w[3] = (q.w[3] << 16) | (q.w[2] >> 16); /* q < 2^103: no bits lost */
    const u128q_u64 up = (U128Q_CAST(u128q_u64, rem) * 65536u + (k - 1u)) / k; /* <= 2^16 */
    tr = u128q_add(tr, u128q_from_halves(up, 0), 0);
    u128q ones;
    ones.w[0] = 0xFFFFFFFFu;
    ones.w[1] = 0xFFFFFFFFu;
    ones.w[2] = 0xFFFFFFFFu;
    ones.w[3] = 0xFFFFFFFFu;
    u128q_u32 rem2 = 0;
    const u128q tovf = u128q_add_u32(u128q_divmod_u32(ones, k, &rem2), 1u, 0);
    return u128q_compare_ge(tr, tovf) ? tovf : tr;
}

#include "collatz/gpu/u128_limbs/u128_limbs_03_lane_loop.h"
#include "collatz/gpu/u128_limbs/u128_limbs_04_split_tables.h"
#include "collatz/gpu/u128_limbs/u128_limbs_05_tail_resume.h"
#include "collatz/gpu/u128_limbs/u128_limbs_06_folds_knobs.h"

/* NOLINTEND(modernize-use-nullptr) */

#endif /* COLLATZ_GPU_U128_LIMBS_H */
