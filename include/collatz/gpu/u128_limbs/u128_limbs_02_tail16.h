/* Per-item cap on the block count. The trajectory can enter the trivial cycle
 * {1, 2} (T(2) = 1, T(1) = 2, so T^16(2) = 2): once a block-start value equals 2
 * while the lower bound n <= 2, the recurrence v = (v>>16)*3^f + c maps 2 -> 2
 * forever and `while (v >= n)` never terminates. Callers use a large n
 * (the trajectory drops below n long before reaching {1, 2}), but a degenerate
 * input must not hang a GPU work-item, since an in-flight kernel cannot be
 * killed. Reaching the cap sets the overflow flag and stops, as a true 2^128
 * overflow does, and the host re-walks the item on the CPU. The cap is far above
 * any trajectory below 2^127, whose total stopping time is a few thousand
 * T-steps, i.e. a few hundred 16-step blocks. */
#define U128Q_TAIL16_MAX_BLOCKS (1u << 20)

/* 96-bit block step, the step of u128q_tail16_resume (for n < 2^86) and of the
 * tail_persistent lane loop (for n < 2^86, u128_limbs_03_lane_loop.h). While
 * the block-start value v < 2^86, v is held as (lo: ulong, hi: uint) with
 * hi < 2^22, and one block jump v' = (v >> 16) * 3^f + c is done in 96 bits:
 *   h = v >> 16 < 2^70, split as h_lo = (lo >> 16) | (hi << 48) (64 bits) and
 *   h_hi = hi >> 16 < 2^6. With k = 3^f <= 3^16 < 2^25.36 and c < 3^16 < 2^27:
 *     p0 = lo32(h_lo) * k < 2^57.4,  p1 = hi32(h_lo) * k < 2^57.4,
 *     p2 = h_hi * k < 2^31.4          (three 32x32 -> 64 products),
 *     v' = p0 + c + (p1 << 32) + (p2 << 64)
 *        = [t = p0 + c] + [(p1 mod 2^32) << 32]      -> new lo, carry out
 *          + ((p1 >> 32) + p2 + carry) << 64         -> new hi.
 *   p0 + c < 2^58 does not wrap. v' = h*k + c < 2^70 * 2^25.36 + 2^27 < 2^96, so
 *   the new hi fits 32 bits: the step cannot overflow inside the guard
 *   hi < 2^22 (the 2^54 argument of u128q_tail16's u64 fast path, one 32-bit
 *   limb up).
 * v >= n becomes U128Q_U96_GE_N (hi != 0 || lo >= n_lo for n < 2^64). The
 * block-start max is kept as two u64 registers (mhi, mlo) = (hi64, lo64) of
 * the u128 max, updated with the same >= rule as the limb loop, so it stays
 * exact even when an earlier limb-path block in the tail pushed the max past
 * 2^86. Values >= 2^86 (hi >= 2^22) and an n >= 2^86 take the 4-limb step,
 * and the MAX_BLOCKS runaway cap is checked in every loop as in u128q_tail16.
 * So (blocks, max, overflow, v_end) are bit-identical to u128q_tail16 (host
 * differential test in test_gpu_u128_limbs: >1e6 random tails plus carry,
 * 2^54, 2^64 and 2^86 edges). */
#define U128Q_U96_HI_LIMIT (1u << 22) /* v < 2^86 <=> v.w[3] == 0 && v.w[2] < 2^22 */

/* Frontier blocks start far above 2^64, so the u96 step also runs for
 * n < 2^86: n is held as (n_hi: uint, n_lo: ulong) like v, and "v >= n" becomes
 * hi > n_hi || (hi == n_hi && lo >= n_lo), which for n < 2^64 (n_hi == 0)
 * reduces to hi != 0 || lo >= n_lo. The block step itself does not depend on
 * n, so (blocks, max, overflow, v_end) stay bit-identical to u128q_tail16. */
#define U128Q_U96_N_OK(n) ((n).w[3] == 0u && (n).w[2] < U128Q_U96_HI_LIMIT)
/* v >= n for v = (hi:lo), n = (n_hi:n_lo), both below 2^86 (u96 lane form). */
#define U128Q_U96_GE_N(hi, lo, n_hi, n_lo) ((hi) > (n_hi) || ((hi) == (n_hi) && (lo) >= (n_lo)))

/* The shared 16-step block recurrence, the same spec on CPU and GPU:
 *
 *   blocks = 0, v = m_a
 *   while v >= n:
 *     r = v mod 2^16
 *     e = TBL16[r]                packed: c = T^16(r) in the high 27 bits,
 *                                 f = odd-step count in the low 5 bits
 *     v = (v >> 16) * 3^f + c     one 16-step block jump
 *     blocks += 1
 *
 * TBL16 packing: e = (c << 5) | f, with c = T^16(r) (< 3^16 < 2^27) and
 * f <= 16 < 2^5. The recurrence stops the first time v < n (it may overshoot
 * below n by up to 16 T-steps within the final block -- this is the coarse,
 * block-granular tail).
 *
 * *max_out receives the maximum block-start value of v observed (an exact
 * lower bound on the excursion; the host turns it into ub = (max*3^16)>>16 to
 * decide which candidates to re-walk exactly on the CPU). *overflow_out is set
 * to 1 iff a block jump's multiply or add exceeded 2^128 or the walk reached
 * U128Q_TAIL16_MAX_BLOCKS blocks with v still >= n (the item then stopped early
 * and must be re-verified on the CPU). *vend_out receives v after the last
 * block. Returns the block count.
 *
 * tbl16 is a flat 2^16-entry table in (device) global memory.
 *
 * u64 fast path: while the block-start value v < 2^54 and n < 2^64, one
 * 16-step block jump v = (v>>16)*3^f + c runs in a single 64-bit word instead
 * of the 4 x uint32 limbs: (v>>16) < 2^38 and 3^f <= 3^16 < 2^25.4, so the
 * product is < 2^63.4 and adding c < 3^16 stays < 2^64 (never overflows).
 * Almost every block is under 2^54; once v reaches 2^54 the loop falls back to
 * the exact limb path for that and later blocks. Block count, max block-start
 * and overflow flag are bit-identical to the pure-limb loop;
 * test_gpu_u128_limbs exercises this path directly on the host. */
U128Q_INLINE u128q_u32 u128q_tail16(u128q n,
                                    u128q v,
                                    U128Q_GLOBAL const u128q_u32* tbl16,
                                    u128q* max_out,
                                    u128q_u32* overflow_out,
                                    u128q* vend_out) {
    u128q_u32 blocks = 0;
    u128q maxv = v;
    u128q_u32 ovf = 0;
    /* The fast path needs n < 2^64; n_lo is the low word of n. */
    const int n_fits_u64 = (n.w[2] == 0u && n.w[3] == 0u);
    const u128q_u64 n_lo = u128q_lo64(n);
    while (u128q_compare_ge(v, n)) {
        u128q_u32 r;
        u128q_u32 e;
        u128q_u32 c;
        u128q_u32 f;
        u128q h;
        u128q_u32 o1 = 0;
        u128q_u32 o2 = 0;
        u128q prod;
        u128q next;
        if (blocks >= U128Q_TAIL16_MAX_BLOCKS) {
            ovf = 1; /* abort: treat a runaway item like a 2^128 overflow */
            break;
        }
        /* Fast path: while v < 2^54 (top limbs clear, w[1] < 2^22) and n fits
         * u64, run the block jump in a single ulong. Bit-identical to the limb
         * loop below: same TBL16 lookup, same v=(v>>16)*3^f+c, same block-start
         * max tracking, and it cannot overflow (see the u64 fast path above). */
        if (n_fits_u64 && v.w[3] == 0u && v.w[2] == 0u && v.w[1] < (1u << 22)) {
            u128q_u64 vv = u128q_lo64(v);
            while ((vv >> 54) == 0u && vv >= n_lo && blocks < U128Q_TAIL16_MAX_BLOCKS) {
                u128q_u32 rr = U128Q_CAST(u128q_u32, vv) & 0xFFFFu;
                u128q_u32 ee = tbl16[rr];
                u128q_u32 cc = ee >> 5;
                u128q_u32 ff = ee & 0x1Fu;
                u128q cur = u128q_from_halves(vv, 0);
                if (u128q_compare_ge(cur, maxv)) {
                    maxv = cur;
                }
                vv = (vv >> 16) * U128Q_CAST(u128q_u64, u128q_pow3(ff)) + U128Q_CAST(u128q_u64, cc);
                ++blocks;
            }
            v = u128q_from_halves(vv, 0);
            continue; /* re-evaluate the outer condition (fall to limb path if big) */
        }
        r = v.w[0] & 0xFFFFu;
        e = tbl16[r];
        c = e >> 5;
        f = e & 0x1Fu;
        h = u128q_shift_right_16(v);
        if (u128q_compare_ge(v, maxv)) {
            maxv = v;
        }
        prod = u128q_mul_u32(h, u128q_pow3(f), &o1);
        next = u128q_add_u32(prod, c, &o2);
        if (o1 != 0 || o2 != 0) {
            ovf = 1;
            break;
        }
        v = next;
        ++blocks;
    }
    *max_out = maxv;
    *overflow_out = ovf;
    *vend_out = v; /* landing value after the last block (< n on descent) */
    return blocks;
}
