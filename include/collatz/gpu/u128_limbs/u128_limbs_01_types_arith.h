#ifdef __OPENCL_VERSION__
/* OpenCL C: uint and ulong are the core 32- and 64-bit types. */
typedef uint u128q_u32;
typedef ulong u128q_u64;
#define U128Q_INLINE inline
#define U128Q_GLOBAL __global
#define U128Q_CAST(T, x) ((T) (x))
#else
#include <cstdint>
typedef std::uint32_t u128q_u32;
typedef std::uint64_t u128q_u64;
#define U128Q_INLINE inline
#define U128Q_GLOBAL
#define U128Q_CAST(T, x) (static_cast<T>(x))
#endif

/* 128-bit unsigned, 4 x uint32 little-endian limbs (w[0] least significant). */
struct u128q {
    u128q_u32 w[4];
};
typedef struct u128q u128q;

/* Compose from two 64-bit halves: value = (hi << 64) | lo. */
U128Q_INLINE u128q u128q_from_halves(u128q_u64 lo, u128q_u64 hi) {
    u128q r;
    r.w[0] = U128Q_CAST(u128q_u32, lo & 0xFFFFFFFFu);
    r.w[1] = U128Q_CAST(u128q_u32, (lo >> 32) & 0xFFFFFFFFu);
    r.w[2] = U128Q_CAST(u128q_u32, hi & 0xFFFFFFFFu);
    r.w[3] = U128Q_CAST(u128q_u32, (hi >> 32) & 0xFFFFFFFFu);
    return r;
}

U128Q_INLINE u128q_u64 u128q_lo64(u128q a) {
    return U128Q_CAST(u128q_u64, a.w[0]) | (U128Q_CAST(u128q_u64, a.w[1]) << 32);
}
U128Q_INLINE u128q_u64 u128q_hi64(u128q a) {
    return U128Q_CAST(u128q_u64, a.w[2]) | (U128Q_CAST(u128q_u64, a.w[3]) << 32);
}

/* r = a + b (mod 2^128). *carry_out (may be null) receives the carry-out of the
 * top limb, i.e. 1 iff the true sum is >= 2^128. */
U128Q_INLINE u128q u128q_add(u128q a, u128q b, u128q_u32* carry_out) {
    u128q r;
    u128q_u64 carry = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        u128q_u64 s = U128Q_CAST(u128q_u64, a.w[i]) + U128Q_CAST(u128q_u64, b.w[i]) + carry;
        r.w[i] = U128Q_CAST(u128q_u32, s & 0xFFFFFFFFu);
        carry = s >> 32;
    }
    if (carry_out) {
        *carry_out = U128Q_CAST(u128q_u32, carry);
    }
    return r;
}

/* r = a + k (mod 2^128), k a 32-bit addend. *carry_out (may be null) gets the
 * top-limb carry-out (1 iff true sum >= 2^128). */
U128Q_INLINE u128q u128q_add_u32(u128q a, u128q_u32 k, u128q_u32* carry_out) {
    u128q r;
    u128q_u64 s = U128Q_CAST(u128q_u64, a.w[0]) + U128Q_CAST(u128q_u64, k);
    u128q_u64 carry;
    int i;
    r.w[0] = U128Q_CAST(u128q_u32, s & 0xFFFFFFFFu);
    carry = s >> 32;
    for (i = 1; i < 4; ++i) {
        u128q_u64 t = U128Q_CAST(u128q_u64, a.w[i]) + carry;
        r.w[i] = U128Q_CAST(u128q_u32, t & 0xFFFFFFFFu);
        carry = t >> 32;
    }
    if (carry_out) {
        *carry_out = U128Q_CAST(u128q_u32, carry);
    }
    return r;
}

/* r = a >> 16 (logical). */
U128Q_INLINE u128q u128q_shift_right_16(u128q a) {
    u128q r;
    int i;
    for (i = 0; i < 3; ++i) {
        u128q_u64 lo = U128Q_CAST(u128q_u64, a.w[i]) >> 16;
        u128q_u64 hi = (U128Q_CAST(u128q_u64, a.w[i + 1]) << 16) & 0xFFFFFFFFu;
        r.w[i] = U128Q_CAST(u128q_u32, lo | hi);
    }
    r.w[3] = a.w[3] >> 16;
    return r;
}

/* r = a * k (mod 2^128), k a 32-bit factor (128x32 -> 128). *overflow (may be
 * null) is set to 1 iff the true product exceeds 2^128 - 1 (top-limb carry). */
U128Q_INLINE u128q u128q_mul_u32(u128q a, u128q_u32 k, u128q_u32* overflow) {
    u128q r;
    u128q_u64 kk = U128Q_CAST(u128q_u64, k);
    u128q_u64 carry = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        u128q_u64 p = U128Q_CAST(u128q_u64, a.w[i]) * kk + carry;
        r.w[i] = U128Q_CAST(u128q_u32, p & 0xFFFFFFFFu);
        carry = p >> 32;
    }
    if (overflow) {
        *overflow = (carry != 0) ? 1u : 0u;
    }
    return r;
}

/* 1 iff a >= b, else 0. */
U128Q_INLINE int u128q_compare_ge(u128q a, u128q b) {
    int i;
    for (i = 3; i >= 0; --i) {
        if (a.w[i] != b.w[i]) {
            return (a.w[i] > b.w[i]) ? 1 : 0;
        }
    }
    return 1;
}

/* Branch-free, memory-free 3^e for e in [0, 16]. The bits of e pick the
 * factors 3^1, 3^2, 3^4, 3^8 (each a select, no load), and e == 16 (the only
 * e <= 16 with bit 4 set, whose low bits are then all zero) returns 3^16
 * directly. So the value equals the table in u128q_pow3 for every e in [0, 16]
 * (checked exhaustively by test_gpu_u128_limbs). A per-lane indexed
 * private array can land in stack or __constant memory, where lanes of one
 * SIMD group with different exponents serialise. */
U128Q_INLINE u128q_u32 u128q_pow3_bf(u128q_u32 e) {
    u128q_u32 p = ((e & 1u) != 0u) ? 3u : 1u;
    p *= ((e & 2u) != 0u) ? 9u : 1u;
    p *= ((e & 4u) != 0u) ? 81u : 1u;
    p *= ((e & 8u) != 0u) ? 6561u : 1u;
    return ((e & 16u) != 0u) ? 43046721u : p;
}

/* The cpi program is built with -DCOLLATZ_GPU_POW3_LOCAL=1, so every
 * u128q_pow3 call there uses u128q_pow3_bf instead of the indexed table. The
 * survivor_tail16 program and host C++ keep the table (and the unit test
 * compares the two). */
#ifndef COLLATZ_GPU_POW3_LOCAL
#define COLLATZ_GPU_POW3_LOCAL 0
#endif

/* 3^e for e in [0, 16]; 3^16 = 43046721 < 2^32. */
U128Q_INLINE u128q_u32 u128q_pow3(u128q_u32 e) {
#if COLLATZ_GPU_POW3_LOCAL
    return u128q_pow3_bf(e);
#else
    const u128q_u32 tbl[17] = {1u,
                               3u,
                               9u,
                               27u,
                               81u,
                               243u,
                               729u,
                               2187u,
                               6561u,
                               19683u,
                               59049u,
                               177147u,
                               531441u,
                               1594323u,
                               4782969u,
                               14348907u,
                               43046721u};
    return tbl[e];
#endif
}
