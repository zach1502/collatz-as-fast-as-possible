
/* Shared Step-2 and coverage helpers, compiled both as OpenCL C and host C++.
 * They are bit-identical to the CPU verifier's fold (see
 * collatz/digest_term.hpp: mix64 / rotl64 / result_digest_term,
 * src/core/verify.cpp: mod_mersenne61, and src/core/verify_fold.hpp: the
 * coverage key) and to Step 2's mod-9 sieve (src/core/step2.cpp:
 * Filter::mod9). The GPU survivor kernel expands each Step-1 survivor's 2^A
 * candidates on the device and folds the coverage checksum with these
 * functions, so a one-bit difference from the CPU definitions would make GPU
 * and CPU verify results disagree. */

/* splitmix64 finalizer, a 64-bit avalanche mix (== mix64 in collatz/digest_term.hpp). */
U128Q_INLINE u128q_u64 u128q_mix64(u128q_u64 z) {
    const u128q_u64 k1 = 0xBF58476D1CE4E5B9UL; /* fits ulong (OpenCL) / ull (C++) */
    const u128q_u64 k2 = 0x94D049BB133111EBUL;
    z = (z ^ (z >> 30)) * k1;
    z = (z ^ (z >> 27)) * k2;
    return z ^ (z >> 31);
}

/* Reduce modulo the Mersenne prime P = 2^61 - 1 via x = (x & P) + (x >> 61),
 * exact for any x < 2^64 (== verify.cpp mod_mersenne61). */
U128Q_INLINE u128q_u64 u128q_mod_mersenne61(u128q_u64 x) {
    const u128q_u64 p = 2305843009213693951UL; /* 2^61 - 1 */
    x = (x & p) + (x >> 61);
    if (x >= p) {
        x -= p;
    }
    return x;
}

/* Reduces a 128-bit value (lo + hi*2^64) modulo the Mersenne prime
 * P = 2^61 - 1 in one step (the tail lanes' raw u128 coverage and digest
 * sums). Since
 * 2^61 == 1 (mod P), the value splits into 61-bit chunks that sum mod P:
 *   x = c0 + c1*2^61 + c2*2^122,  so  x mod P = (c0 + c1 + c2) mod P.
 * For x < 2^128: c0 = bits[0,61) = lo & P; c1 = bits[61,122) =
 * (lo >> 61) | ((hi & (2^58-1)) << 3) (lo bits 61..63 plus hi bits 0..57, 61
 * bits total); c2 = bits[122,128) = hi >> 58 (6 bits). Each chunk < 2^61, so
 * c0 + c1 + c2 < 3*2^61 < 2^63, and one u64 fold (u128q_mod_mersenne61, exact
 * for any argument < 2^64) closes it.
 *
 * The raw sum gives the per-term fold's residue: folding coverage per term as
 *   cov = mod61(cov + mod61(term_i)),  i.e. cov = Sum_i mod61(term_i)  (mod P),
 * and reduction mod P is a ring homomorphism, so Sum_i (term_i mod P) ==
 * (Sum_i term_i) mod P. A tail lane accumulates the raw term_i (each < 2^64)
 * into a u128 (lo + hi with carry) and reduces the whole sum mod P once via
 * this function: the same residue < P that the per-term reduction produces,
 * which then enters the cross-lane tree reduction. No overflow: one term is
 * added per retired candidate; cand_count is a uint (< 2^32), so a lane sums at
 * most < 2^32 terms each < 2^64, i.e. < 2^96 < 2^128, and the u128 never wraps. */
U128Q_INLINE u128q_u64 u128q_mod_mersenne61_u128(u128q_u64 lo, u128q_u64 hi) {
    const u128q_u64 p = 2305843009213693951UL;                      /* 2^61 - 1 */
    const u128q_u64 mask58 = (U128Q_CAST(u128q_u64, 1) << 58) - 1u; /* 2^58 - 1 */
    const u128q_u64 c0 = lo & p;
    const u128q_u64 c1 = (lo >> 61) | ((hi & mask58) << 3);
    const u128q_u64 c2 = hi >> 58;
    return u128q_mod_mersenne61(c0 + c1 + c2);
}

/* Coverage key of a candidate n: lo64(n) ^ rotl64(hi64(n), 32)
 * (== verify_fold.hpp: key = lo64(n) ^ rotl64(hi64(n), 32)). */
U128Q_INLINE u128q_u64 u128q_cov_key(u128q n) {
    const u128q_u64 lo = u128q_lo64(n);
    const u128q_u64 hi = u128q_hi64(n);
    const u128q_u64 rot = (hi << 32) | (hi >> 32); /* rotl64(hi, 32) */
    return lo ^ rot;
}

/* Per-candidate pow-digest term (== result_digest_term in collatz/digest_term.hpp).
 * Binds the whole (n, blocks, v_end) triple through one splitmix64 finalizer over
 * a 64-bit combination of the triple (rotates + the two splitmix64 multipliers)
 * so any single wrong triple, or a pair of compensating +/-1 block errors, changes
 * the summed digest. */
U128Q_INLINE u128q_u64 u128q_result_digest_term(u128q n, u128q_u64 blocks, u128q v_end) {
    const u128q_u64 g2 = 0xBF58476D1CE4E5B9UL;
    const u128q_u64 g3 = 0x94D049BB133111EBUL;
    const u128q_u64 hn = u128q_hi64(n);
    const u128q_u64 hv = u128q_hi64(v_end);
    const u128q_u64 rn = (hn << 29) | (hn >> 35); /* rotl64(hi64(n), 29) */
    const u128q_u64 rv = (hv << 43) | (hv >> 21); /* rotl64(hi64(v_end), 43) */
    const u128q_u64 key = u128q_lo64(n) ^ rn ^ (blocks * g2) ^ (u128q_lo64(v_end) * g3) ^ rv;
    return u128q_mix64(key);
}

/* The coverage key and the pow-digest term of a candidate n < 2^64 (the cpi
 * program uses them only under COLLATZ_GPU_TAIL_U64N, which the host sets only
 * when every n < 2^64). With hi64(n) == 0, rotl64(hi64(n), k) == 0, so both
 * are the u128q functions above with those terms dropped, and give the same
 * values (test_gpu_u128_limbs). */
U128Q_INLINE u128q_u64 u128q_cov_key_n64(u128q_u64 n) {
    return n;
}
U128Q_INLINE u128q_u64 u128q_result_digest_term_n64(u128q_u64 n, u128q_u64 blocks, u128q v_end) {
    const u128q_u64 g2 = 0xBF58476D1CE4E5B9UL;
    const u128q_u64 g3 = 0x94D049BB133111EBUL;
    const u128q_u64 hv = u128q_hi64(v_end);
    const u128q_u64 rv = (hv << 43) | (hv >> 21); /* rotl64(hi64(v_end), 43) */
    const u128q_u64 key = n ^ (blocks * g2) ^ (u128q_lo64(v_end) * g3) ^ rv;
    return u128q_mix64(key);
}

/* n mod 9 from the 32-bit limbs: 2^32 == 4, 2^64 == 7 and 2^96 == 1 (mod 9)
 * (2^6 == 1), so n == w0 + 4*w1 + 7*w2 + w3 (mod 9). With each limb reduced
 * first the sum is at most 8 + 4*8 + 7*8 + 8 = 104, so every operation is a
 * 32-bit one (== step2.cpp Filter::mod9, which folds 2^64 == 7 on the 64-bit
 * halves). */
U128Q_INLINE u128q_u32 u128q_mod9(u128q n) {
    const u128q_u32 s = n.w[0] % 9u + 4u * (n.w[1] % 9u) + 7u * (n.w[2] % 9u) + n.w[3] % 9u;
    return s % 9u;
}

/* r = base + ((u128)a << shift), a a 32-bit value, shift < 96. Forms the
 * candidate n = n0 + a*2^M on the device. a < 2^A <= 2^14, so
 * a << (shift mod 32) < 2^45 spans at most two limbs. */
U128Q_INLINE u128q u128q_add_shl_u32(u128q base, u128q_u32 a, u128q_u32 shift) {
    u128q s;
    u128q_u32 word = shift >> 5;
    u128q_u32 off = shift & 31u;
    u128q_u64 val = U128Q_CAST(u128q_u64, a) << off; /* a<<off < 2^45, fits ulong */
    u128q_u32 carry_dummy = 0;
    s.w[0] = 0;
    s.w[1] = 0;
    s.w[2] = 0;
    s.w[3] = 0;
    if (word < 4) {
        s.w[word] = U128Q_CAST(u128q_u32, val & 0xFFFFFFFFu);
    }
    if (word + 1 < 4) {
        s.w[word + 1] = U128Q_CAST(u128q_u32, (val >> 32) & 0xFFFFFFFFu);
    }
    return u128q_add(base, s, &carry_dummy);
}

/* Per-batch self-check, shared by the kernels and the host check.
 * sc_term: 32-bit tag of one packed candidate id under a dispatch nonce; the
 * producer (mask/expand) and the tail each sum it, so a stale or short candidate
 * hand-off shows up as a mismatch. sc_fold chains a value into a record hash. */
U128Q_INLINE u128q_u32 u128q_sc_term(u128q_u32 packed, u128q_u32 nonce) {
    u128q_u32 x = packed ^ U128Q_CAST(u128q_u32, nonce * 0x9E3779B9u);
    x ^= x >> 16;
    x = U128Q_CAST(u128q_u32, x * 0x85EBCA6Bu);
    x ^= x >> 13;
    x = U128Q_CAST(u128q_u32, x * 0xC2B2AE35u);
    x ^= x >> 16;
    return x;
}

U128Q_INLINE u128q_u64 u128q_sc_fold(u128q_u64 h, u128q_u64 v) {
    return u128q_mix64(h ^ v);
}

/* Hash of one tail work-group record: its fold partials plus the self-check
 * words (claimed<<32 | flagged, flagsum<<32 | idsum), keyed by the nonce. */
U128Q_INLINE u128q_u64 u128q_sc_group(u128q_u32 nonce,
                                      u128q_u64 cov,
                                      u128q_u64 dig,
                                      u128q_u64 blk,
                                      u128q_u32 cnt,
                                      u128q mx,
                                      u128q_u64 w1,
                                      u128q_u64 w2) {
    u128q_u64 h = u128q_mix64(U128Q_CAST(u128q_u64, nonce) ^ 0x243F6A8885A308D3UL);
    h = u128q_sc_fold(h, cov);
    h = u128q_sc_fold(h, dig);
    h = u128q_sc_fold(h, blk);
    h = u128q_sc_fold(h, U128Q_CAST(u128q_u64, cnt));
    h = u128q_sc_fold(h, u128q_lo64(mx));
    h = u128q_sc_fold(h, u128q_hi64(mx));
    h = u128q_sc_fold(h, w1);
    return u128q_sc_fold(h, w2);
}

/* Hash of one survivor_tail16 per-item record, keyed by nonce and item index. */
U128Q_INLINE u128q_u64 u128q_sc_item(u128q_u32 nonce,
                                     u128q_u32 gid,
                                     u128q_u64 cov,
                                     u128q_u64 dig,
                                     u128q_u64 blk,
                                     u128q_u32 cand,
                                     u128q mx,
                                     u128q_u32 ovf,
                                     u128q_u32 pc) {
    u128q_u64 h = u128q_mix64((U128Q_CAST(u128q_u64, nonce) << 32) ^ U128Q_CAST(u128q_u64, gid));
    h = u128q_sc_fold(h, cov);
    h = u128q_sc_fold(h, dig);
    h = u128q_sc_fold(h, blk);
    h = u128q_sc_fold(h, u128q_lo64(mx));
    h = u128q_sc_fold(h, u128q_hi64(mx));
    return u128q_sc_fold(h,
                         (U128Q_CAST(u128q_u64, cand) << 32) ^ (U128Q_CAST(u128q_u64, ovf) << 16) ^
                             U128Q_CAST(u128q_u64, pc));
}

/* q(v) = floor(v / 2^54) of a lane-loop value v = (hi:lo) < 2^86, which fits a
 * u32 (hi < 2^22, so hi << 10 < 2^32). The slot loop keeps the block-start max
 * of a chain as q(max) (Q54_FLAG in survivor_cpi.cl). */
U128Q_INLINE u128q_u32 u128q_x54(u128q_u64 lo, u128q_u32 hi) {
    return U128Q_CAST(u128q_u32, lo >> 54) | (hi << 10);
}

/* The lower bound x * 2^54 of a max whose q() is x (exact: q(x * 2^54) = x). */
U128Q_INLINE u128q u128q_from_x54(u128q_u32 x) {
    return u128q_from_halves(U128Q_CAST(u128q_u64, x) << 54, U128Q_CAST(u128q_u64, x) >> 10);
}

/* q(mx) >= (t_hi:t_lo), i.e. floor(mx / 2^54) >= the precomputed floor(thr / 2^54). */
U128Q_INLINE int u128q_ge_q54(u128q mx, u128q_u64 t_lo, u128q_u64 t_hi) {
    const u128q_u64 lo = u128q_lo64(mx);
    const u128q_u64 hi = u128q_hi64(mx);
    const u128q_u64 q_lo = (lo >> 54) | (hi << 10);
    const u128q_u64 q_hi = hi >> 54;
    return (q_hi > t_hi || (q_hi == t_hi && q_lo >= t_lo)) ? 1 : 0;
}

/* One u96 block jump of a lane-form chain v = (hi:lo) < 2^86 with {3^f, c}
 * from u128q_split_kc: v' = (v >> 16) * 3^f + c in 96 bits (the step and its
 * overflow bound at U128Q_U96_HI_LIMIT), and x = max(x, q(block start)) in
 * place of the 96-bit block-start max. The walk does not read the max, and x
 * equals q of the exact 96-bit block-start max because q is monotone
 * (test_gpu_u128_limbs). x costs one shift-or and one u32 max per block. */
U128Q_INLINE void u128q_split_step_maxhi(U128Q_SPLIT_Q const u128q_u32* tab,
                                         u128q_u64* lo,
                                         u128q_u32* hi,
                                         u128q_u32* mx,
                                         u128q_u32* blocks) {
    u128q_u64 k = 0;
    u128q_u64 cc = 0;
    const u128q_u64 l = *lo;
    const u128q_u32 h = *hi;
    u128q_split_kc(tab, U128Q_CAST(u128q_u32, l) & 0xFFFFu, &k, &cc);
    const u128q_u32 x = u128q_x54(l, h);
    *mx = x > *mx ? x : *mx;
    const u128q_u64 hi64 = U128Q_CAST(u128q_u64, h);
    const u128q_u64 h_lo = (l >> 16) | (hi64 << 48);
    const u128q_u64 p0 = (h_lo & 0xFFFFFFFFu) * k;
    const u128q_u64 p1 = (h_lo >> 32) * k;
    const u128q_u64 p2 = U128Q_CAST(u128q_u64, h >> 16) * k;
    const u128q_u64 t = p0 + cc;
    const u128q_u64 nlo = t + (p1 << 32);
    const u128q_u64 carry = (nlo < t) ? 1u : 0u;
    *hi = U128Q_CAST(u128q_u32, (p1 >> 32) + p2 + carry);
    *lo = nlo;
    *blocks += 1u;
}

/* The coalesced mask's per-lane mod-9 pattern. Bit j of the 32-bit word is
 * set iff (r + j*p) mod 9 is in {0,1,3,6,7} (bit r of 0xCB), for the lane's
 * start residue r and p = 2^M mod 9. The word depends only on (p, r), so the
 * table holds a row of 9 entries per p. */
#ifdef __OPENCL_VERSION__
#define U128Q_CONST_TAB __constant
#else
#define U128Q_CONST_TAB inline constexpr
#endif
/* Row p (p = 0..8; p in {1,2,4,5,7,8} occurs), column r = start residue. */
U128Q_CONST_TAB u128q_u32 u128q_mod9_pat[81] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0x00000000u, 0x5B2D96CBu, 0x2D96CB65u, 0x96CB65B2u, 0xCB65B2D9u, 0x65B2D96Cu,
    0xB2D96CB6u, 0xD96CB65Bu, 0x6CB65B2Du, 0xB65B2D96u, 0x4DA6D369u, 0xDA6D369Bu, 0xA6D369B4u,
    0x6D369B4Du, 0xD369B4DAu, 0x369B4DA6u, 0x69B4DA6Du, 0x9B4DA6D3u, 0xB4DA6D36u, 0xFFFFFFFFu,
    0x6DB6DB6Du, 0x00000000u, 0xFFFFFFFFu, 0xB6DB6DB6u, 0x00000000u, 0xFFFFFFFFu, 0xDB6DB6DBu,
    0x00000000u, 0xCB65B2D9u, 0x2D96CB65u, 0xB65B2D96u, 0xD96CB65Bu, 0x65B2D96Cu, 0x96CB65B2u,
    0x5B2D96CBu, 0x6CB65B2Du, 0xB2D96CB6u, 0x69B4DA6Du, 0xDA6D369Bu, 0x369B4DA6u, 0x4DA6D369u,
    0xD369B4DAu, 0xB4DA6D36u, 0x6D369B4Du, 0x9B4DA6D3u, 0xA6D369B4u, 0xFFFFFFFFu, 0xDB6DB6DBu,
    0x00000000u, 0xFFFFFFFFu, 0xB6DB6DB6u, 0x00000000u, 0xFFFFFFFFu, 0x6DB6DB6Du, 0x00000000u,
    0xD96CB65Bu, 0x2D96CB65u, 0xB2D96CB6u, 0x5B2D96CBu, 0x65B2D96Cu, 0xB65B2D96u, 0xCB65B2D9u,
    0x6CB65B2Du, 0x96CB65B2u, 0x6D369B4Du, 0xDA6D369Bu, 0xB4DA6D36u, 0x69B4DA6Du, 0xD369B4DAu,
    0xA6D369B4u, 0x4DA6D369u, 0x9B4DA6D3u, 0x369B4DA6u};
