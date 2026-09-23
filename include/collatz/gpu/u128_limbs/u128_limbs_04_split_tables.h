
/* Split tables: the 16-step {3^f, c} of a block composed from two 8-step
 * lookups in one 256-entry sub-table, so the per-block lookup is an on-chip
 * (__local) access instead of a 256 KiB __global TBL16 gather. Every block is
 * 16 steps, as with TBL16, so blocks, exits, folds and the digest are the same.
 *
 * Sub-table of width w (2^w entries): entry i = {3^f, c} with
 *   2^w T^w(n) = 3^f n + c   for every n == i (mod 2^w)
 * (T the shortcut map, n -> n/2 or (3n+1)/2; the parity word of T^0..T^{w-1}
 * depends only on n mod 2^w, as for TBL16). Composition: after s steps with
 * 2^s T^s(n) = K n + C, the next w steps are indexed by
 *   T^s(n) mod 2^w = ((K n + C) >> s) mod 2^w,
 * which depends only on (K r + C) mod 2^{s+w}, r = n mod 2^16 (s + w <= 16),
 * so it is computed in wrapping 32-bit arithmetic from r alone. With that
 * entry {k2, c2}:  2^{s+w} T^{s+w}(n) = k2 (K n + C) + 2^s c2, i.e.
 *   K' = K k2,  C' = k2 C + (c2 << s).
 * After 16 steps K = 3^f and TBL16's constant is c16 = T^16(r) =
 * (K r + C) >> 16 (the division is exact). K <= 3^16 < 2^32 and C only grows
 * (k2 >= 1), up to its final value <= 2^{16-f}(3^f - 2^f) < 2^26, so no 32-bit
 * C product wraps; K r < 2^42 is the one 64-bit product.
 * test_gpu_u128_limbs checks the composition against TBL16 for all 2^16
 * residues.
 *
 * The table image is the 256 width-8 entries as u32 pairs {3^f, c} (2048 B),
 * used for both 8-step halves of a block.
 */
#ifdef __OPENCL_VERSION__
#define U128Q_SPLIT_Q __local
#else
#define U128Q_SPLIT_Q
#endif

/* Entries (u32 pairs) of the split's table image. */
U128Q_INLINE u128q_u32 u128q_split_entries(void) {
    return 256u;
}

/* One composition stage: fold the width-w entry indexed by the next w steps
 * into (K, C) after s steps (see above). */
U128Q_INLINE void u128q_split_stage(U128Q_SPLIT_Q const u128q_u32* tab,
                                    u128q_u32 base,
                                    u128q_u32 w,
                                    u128q_u32 s,
                                    u128q_u32 r,
                                    u128q_u32* kk,
                                    u128q_u32* cc) {
    const u128q_u32 i = (((*kk) * r + (*cc)) >> s) & ((1u << w) - 1u);
#ifdef __OPENCL_VERSION__
    const uint2 t = vload2(base + i, tab); /* one 8-byte on-chip load */
    const u128q_u32 k2 = t.x;
    const u128q_u32 c2 = t.y;
#else
    const u128q_u32 k2 = tab[2u * (base + i)];
    const u128q_u32 c2 = tab[2u * (base + i) + 1u];
#endif
    *cc = k2 * (*cc) + (c2 << s);
    *kk = (*kk) * k2;
}

/* {3^f, c} of TBL16 entry r (r < 2^16), composed from the two 8-step stages. */
U128Q_INLINE void
u128q_split_kc(U128Q_SPLIT_Q const u128q_u32* tab, u128q_u32 r, u128q_u64* k, u128q_u64* c) {
    u128q_u32 kk = 1u;
    u128q_u32 cc = 0u;
    const u128q_u32 w1 = 8u;
    const u128q_u32 base2 = 0u;
    /* first stage from (1, 0): index r mod 2^w1, no multiply needed */
    const u128q_u32 i1 = r & ((1u << w1) - 1u);
#ifdef __OPENCL_VERSION__
    const uint2 t1 = vload2(i1, tab);
    kk = t1.x;
    cc = t1.y;
#else
    kk = tab[2u * i1];
    cc = tab[2u * i1 + 1u];
#endif
    /* second stage: the same width-8 table, after 8 steps */
    u128q_split_stage(tab, base2, 16u - w1, w1, r, &kk, &cc);
    /* TBL16 stores c16 = T^16(r) = (3^f r + C) / 2^16 (exact), the constant
     * of T^16(n) = 3^f (n >> 16) + c16; 3^f r < 2^42, so one 32x32->64
     * product. */
    *k = U128Q_CAST(u128q_u64, kk);
    *c = (U128Q_CAST(u128q_u64, kk) * r + cc) >> 16;
}

#ifndef __OPENCL_VERSION__
/* Host: {3^f, c} of the width-w sub-table entry i, by walking w shortcut steps
 * of the affine map (independent of TBL16, which the unit test compares). */
inline void u128q_split_sub(u128q_u32 w, u128q_u32 i, u128q_u32* k, u128q_u32* c) {
    u128q_u64 x = i;
    u128q_u32 kk = 1u;
    u128q_u32 cc = 0u;
    for (u128q_u32 s = 0; s < w; ++s) {
        if ((x & 1u) != 0u) {
            kk *= 3u;
            cc = 3u * cc + (1u << s); /* 2^{s+1} T^{s+1} = 3 (K n + C) + 2^s */
            x = (3u * x + 1u) / 2u;
        } else {
            x /= 2u;
        }
    }
    *k = kk;
    *c = cc;
}

/* Host: fill the split's table image (2 * u128q_split_entries() words). */
inline void u128q_split_fill(u128q_u32* out) {
    const u128q_u32 w1 = 8u;
    const u128q_u32 n1 = 1u << w1;
    for (u128q_u32 i = 0; i < n1; ++i) {
        u128q_split_sub(w1, i, &out[2u * i], &out[2u * i + 1u]);
    }
}

#endif
