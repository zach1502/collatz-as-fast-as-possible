/* One work-item per Step-1 survivor runs Step 2 and Step 3 on the GPU, so only
 * survivors (not their 2^A expanded candidates) cross the host/device bus (the
 * GPU design of arXiv:2602.10466, section 6).
 *
 * Per work-item, for its survivor class (n0, m = T^M(n0), f):
 *   (Step 2) reconstruct the 2^A-bit candidate mask exactly as the CPU Filter:
 *            bit a set iff  BV_i[(j0 + a) mod 2^B] = 1               (lookahead)
 *                      AND  (n0 + a*2^M) mod 9 not in {2,4,5,8}      (mod-9)
 *                      AND  a < a_star                               (partial OEE)
 *            where j0 = (m mod 2^B) * inv3f mod 2^B, i = f - fmin.
 *   (Step 3) for each set bit a, form n = n0 + a*2^M and m_a = m + 3^f*a, run
 *            the shared 16-step block recurrence (u128q_tail16) and accumulate:
 *              - candidate count (after the n < preflimit preflight skip),
 *              - wrapping u64 sum of block counts,
 *              - coverage fold: sum mod 2^61-1 of mod_mersenne61(mix64(key(n))),
 *              - pow digest: sum mod 2^61-1 of result_digest_term(n, blocks, v_end),
 *              - the max block-start value over all this item's candidates,
 *              - an overflow flag (u128 overflow or the runaway cap); a
 *                candidate that sets it adds nothing to the folds above (the
 *                host folds it from an exact replay).
 *
 * The runtime (survivor_runtime.cpp) prepends include/collatz/gpu/u128_limbs.h
 * before the build, so the u128q type and its helpers are in scope.
 *
 * 128-bit values cross as uint4 limbs, little-endian (.x,.y,.z,.w)==(w0,w1,w2,w3).
 * f is passed per item (inv3f, the BV word base, the all-ones mode flag and the
 * 3^f limbs are precomputed on the host from f), so a batch may mix f values.
 */

/* The Step-2 bitvector table is uploaded across up to 4 device buffers (each a
 * power-of-two run of levels) so no allocation exceeds CL_DEVICE_MAX_MEM_ALLOC_SIZE.
 * Word W lives in buffer W>>bv_lg at offset W&((1<<bv_lg)-1); a single-buffer run
 * passes bv_lg large enough that every W maps to bv0. */
inline uint bv_word(__global const uint* bv0,
                    __global const uint* bv1,
                    __global const uint* bv2,
                    __global const uint* bv3,
                    uint bv_lg,
                    uint w) {
    const uint off = w & ((1u << bv_lg) - 1u);
    const uint buf = w >> bv_lg;
    return buf == 0u ? bv0[off] : buf == 1u ? bv1[off] : buf == 2u ? bv2[off] : bv3[off];
}

__kernel void survivor_tail16(__global const uint* tbl16,     /* 0: 2^16 packed entries */
                              __global const uint* bv0,       /* 1: bitvector buffer 0 */
                              __global const uint4* n0_in,    /* 2: per-item n0 */
                              __global const uint4* m_in,     /* 3: per-item m = T^M(n0) */
                              __global const uint* astar_in,  /* 4: per-item a_star (twoA = none) */
                              __global const uint* inv3f_in,  /* 5: per-item inv(3^f) mod 2^B */
                              __global const uint* bvbase_in, /* 6: per-item BV word offset */
                              __global const uint* bvmode_in, /* 7: per-item 1=all-ones window */
                              __global const uint4* p3f_in,   /* 8: per-item 3^f limbs */
                              uint count,                     /* 9: live item count */
                              uint b_mask,                    /* 10: 2^B - 1 */
                              uint twoA,                      /* 11: 2^A */
                              uint m_shift,                   /* 12: M = N - A */
                              uint pow2m_mod9,                /* 13: 2^M mod 9 */
                              uint4 preflimit,                /* 14: skip n < preflimit */
                              __global ulong* cov_out,        /* 15: coverage partial (mod p) */
                              __global ulong* blocks_out,     /* 16: wrapping block sum */
                              __global uint* cand_out,        /* 17: candidates processed */
                              __global uint4* maxv_out,       /* 18: max block-start value */
                              __global uint* ovf_out,         /* 19: overflow/cap flag */
                              __global uint* pc_out,          /* 20: mask popcount (masked_out) */
                              __global ulong* dig_out,        /* 21: pow-digest partial (mod p) */
                              __global const uint* bv1,       /* 22: bitvector buffer 1 */
                              __global const uint* bv2,       /* 23: bitvector buffer 2 */
                              __global const uint* bv3,       /* 24: bitvector buffer 3 */
                              uint bv_lg,                     /* 25: log2 words/buffer */
                              uint nonce,                     /* 26: self-check dispatch nonce */
                              __global ulong* chk_out) {      /* 27: per-item self-check hash */
    uint gid = get_global_id(0);
    if (gid >= count) {
        return;
    }

    uint4 n0v = n0_in[gid];
    uint4 mv = m_in[gid];
    u128q n0;
    u128q m;
    u128q pf;
    n0.w[0] = n0v.x;
    n0.w[1] = n0v.y;
    n0.w[2] = n0v.z;
    n0.w[3] = n0v.w;
    m.w[0] = mv.x;
    m.w[1] = mv.y;
    m.w[2] = mv.z;
    m.w[3] = mv.w;
    pf.w[0] = preflimit.x;
    pf.w[1] = preflimit.y;
    pf.w[2] = preflimit.z;
    pf.w[3] = preflimit.w;

    const uint astar = astar_in[gid];
    const ulong inv3f = (ulong) inv3f_in[gid];
    const uint bvbase = bvbase_in[gid];
    const uint bvmode = bvmode_in[gid];
    uint4 p3fv = p3f_in[gid];
    u128q p3f;
    p3f.w[0] = p3fv.x;
    p3f.w[1] = p3fv.y;
    p3f.w[2] = p3fv.z;
    p3f.w[3] = p3fv.w;

    /* j0 = (m mod 2^B) * inv3f mod 2^B (matches Filter::mask). */
    const ulong m_res = u128q_lo64(m) & (ulong) b_mask;
    const ulong j0 = (m_res * inv3f) & (ulong) b_mask;
    const uint r0 = u128q_mod9(n0);

    ulong cov = 0;
    ulong dig = 0; /* pow digest (mod p) */
    ulong blocks = 0;
    uint cand = 0;
    uint pc = 0;
    uint ovf = 0;
    u128q maxv;
    maxv.w[0] = 0;
    maxv.w[1] = 0;
    maxv.w[2] = 0;
    maxv.w[3] = 0;

    for (uint a = 0; a < twoA; ++a) {
        /* Step-2 lookahead bit. */
        uint bvbit;
        if (bvmode != 0) {
            bvbit = 1u;
        } else {
            const ulong p = (j0 + (ulong) a) & (ulong) b_mask;
            const uint word = bv_word(bv0, bv1, bv2, bv3, bv_lg, bvbase + (uint) (p >> 5));
            bvbit = (word >> (uint) (p & 31u)) & 1u;
        }
        if (bvbit == 0u) {
            continue;
        }
        /* Step-2 mod-9 preimage sieve: exclude (n0 + a*2^M) mod 9 in {2,4,5,8}. */
        const uint r = (r0 + a * pow2m_mod9) % 9u;
        if (r == 2u || r == 4u || r == 5u || r == 8u) {
            continue;
        }
        /* Step-2 partial-OEE prefix: keep a < a_star. */
        if (a >= astar) {
            continue;
        }
        ++pc; /* mask bit set (matches CandidateMask::popcount, pre-preflight). */

        /* n = n0 + a*2^M; skip candidates already discharged by the preflight. */
        const u128q n = u128q_add_shl_u32(n0, a, m_shift);
        if (u128q_compare_ge(n, pf) == 0) {
            continue;
        }

        /* m_a = m + 3^f * a. */
        uint o1 = 0;
        const u128q prod = u128q_mul_u32(p3f, a, &o1);
        uint o2 = 0;
        const u128q m_a = u128q_add(m, prod, &o2);

        u128q cmax;
        u128q vend;
        uint covf = 0;
        const uint blk = u128q_tail16(n, m_a, tbl16, &cmax, &covf, &vend);
        if (u128q_compare_ge(cmax, maxv) != 0) {
            maxv = cmax;
        }
        if (o1 != 0u || o2 != 0u || covf != 0u) {
            /* Left u128 or hit the block cap: flag the survivor and fold
             * nothing; the host re-walks it and folds this candidate from its
             * exact replay (verify_gpu_fold.cpp process_job). */
            ovf = 1u;
            continue;
        }
        blocks += (ulong) blk;
        ++cand;

        /* Coverage fold (mod 2^61-1), identical to the CPU verifier. */
        const ulong key = u128q_cov_key(n);
        const ulong term = u128q_mod_mersenne61(u128q_mix64(key));
        cov = u128q_mod_mersenne61(cov + term);

        /* Pow-digest fold (mod 2^61-1), identical to the CPU. */
        const ulong dterm = u128q_mod_mersenne61(u128q_result_digest_term(n, (ulong) blk, vend));
        dig = u128q_mod_mersenne61(dig + dterm);
    }

    cov_out[gid] = cov;
    blocks_out[gid] = blocks;
    cand_out[gid] = cand;
    maxv_out[gid] = (uint4) (maxv.w[0], maxv.w[1], maxv.w[2], maxv.w[3]);
    ovf_out[gid] = ovf;
    pc_out[gid] = pc;
    dig_out[gid] = dig;
    chk_out[gid] = u128q_sc_item(nonce, gid, cov, dig, blocks, cand, maxv, ovf, pc);
}
