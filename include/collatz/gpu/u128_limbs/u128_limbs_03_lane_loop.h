/* Pieces of the tail_persistent slot loop (survivor_cpi.cl), which walks one
 * chain per slot:
 *   u128q_lane_fast_ok     -- may this (n, v) start in the u96 lane loop?
 *                             (u128q_lane_fast_ok_n86 without U64N)
 *   u128q_lane_cond        -- the u96 inner-loop condition, per chain
 *                             (u128q_lane_cond_n86 without U64N)
 *   u128q_split_step_maxhi -- one u96 block jump with the q54 block-start max
 *                             (u128_limbs_06_folds_knobs.h)
 *   u128q_tail16_resume    -- the u96/limb recurrence from a mid-walk state
 *                             (u128_limbs_05_tail_resume.h)
 * Composed as the slot loop composes them, they return what u128q_tail16
 * returns: the same blocks, overflow flag and v_end, and a block-start max with
 * the same floor(. / 2^54) that does not exceed the true one (Q54_FLAG
 * in survivor_cpi.cl). test_gpu_u128_limbs checks the composition and resumes
 * at every stop point against u128q_tail16. */

/* 1 iff (n, v) may start in the u96 lane loop: n < 2^64, v < 2^86 and v >= n,
 * the guard of the u96 step plus "at least one block to walk". Anything else
 * (v < n: 0 blocks; v >= 2^86 or n >= 2^64) is walked by u128q_tail16_resume
 * from the start. */
U128Q_INLINE int u128q_lane_fast_ok(u128q n, u128q v) {
    return n.w[2] == 0u && n.w[3] == 0u && v.w[3] == 0u && v.w[2] < U128Q_U96_HI_LIMIT &&
           (v.w[2] != 0u || u128q_lo64(v) >= u128q_lo64(n));
}

/* The u96 lane loop's condition for a chain in lane form: v = (hi:lo),
 * hi < 2^22 on entry, n = n_lo < 2^64. It holds while v < 2^86, v >= n and the
 * MAX_BLOCKS cap is not reached. */
U128Q_INLINE int u128q_lane_cond(u128q_u64 lo, u128q_u32 hi, u128q_u64 n_lo, u128q_u32 blocks) {
    return hi < U128Q_U96_HI_LIMIT && (hi != 0u || lo >= n_lo) && blocks < U128Q_TAIL16_MAX_BLOCKS;
}

/* The lane loop for n >= 2^64: the cpi program built without
 * COLLATZ_GPU_TAIL_U64N, which the host does when some n of the run reaches
 * 2^64, and then every n of the run (a block above 2^64) is >= 2^64. The slot
 * holds n as (n_hi: uint, n_lo: ulong) next to v = (hi: uint, lo: ulong), and
 * v >= n is U128Q_U96_GE_N. A chain starts in the loop iff n <= v < 2^86
 * (u128q_lane_fast_ok_n86). No v passes that test once n >= 2^86, so every
 * chain with n >= 2^86 (or with v >= 2^86, or v < n) runs u128q_tail16_resume
 * from the start, as before.
 * Bounds, for n < 2^86 and a block start v < 2^86:
 *  - Step: v' = floor(v / 2^16) * 3^f + c with f <= 16 and
 *    c = T^16(v mod 2^16) <= 3^f - 1 (TBL16's largest constant for each f;
 *    3^16 - 1 at r = 2^16 - 1). So v' <= (3/2)^16 * v + 3^16 - 1 and
 *    v' <= (floor(v / 2^16) + 1) * 3^16 - 1 <= 2^70 * 3^16 - 1 < 2^95.36.
 *    v' is exact in (hi, lo), with hi <= 64 * 3^16 - 1 < 2^32 (no wrap in
 *    u128q_split_step_maxhi; see U128Q_U96_HI_LIMIT).
 *  - Max: the loop keeps x = floor(v / 2^54) = (lo >> 54) | (hi << 10) of each
 *    block start in a uint, which is exact iff v < 2^86 (hi < 2^22). That is
 *    the binding limit: the step alone stays below 2^96 for every
 *    v < 2^16 * floor(2^96 / 3^16), about 2^86.64. So the loop guard, checked
 *    before each block, is hi < 2^22.
 *  - Compare: for n < 2^86 (n_hi < 2^22) and v < 2^96 the lexicographic test
 *    (hi, lo) >= (n_hi, n_lo) is v >= n. That holds for the loop condition and
 *    for the exit test, which sends a chain still >= n to u128q_tail16_resume
 *    (with the full n) and retires one that dropped below n.
 *  - Exit: the loop leaves at v >= 2^86 (still exact, v < 2^96), at v < n or at
 *    the MAX_BLOCKS cap, as the n < 2^64 loop does.
 * The block step and the q54 max do not depend on n, so (blocks, overflow,
 * v_end) equal u128q_tail16's and the max meets Q54_FLAG, as for n < 2^64.
 * test_gpu_u128_limbs checks n = 2^86 - 1, 2^86, 2^86 + 1 and 2^64 - 1,
 * 2^64, 2^64 + 1 with every residue mod 2^16 next to them, and random n in
 * [2^64, 2^96), against GMP and against u128q_tail16_resume from the start. */
U128Q_INLINE int u128q_lane_fast_ok_n86(u128q n, u128q v) {
    return U128Q_U96_N_OK(n) && v.w[3] == 0u && v.w[2] < U128Q_U96_HI_LIMIT &&
           U128Q_U96_GE_N(v.w[2], u128q_lo64(v), n.w[2], u128q_lo64(n));
}

/* u128q_lane_cond for n = (n_hi:n_lo) < 2^86: v < 2^86, v >= n and the
 * MAX_BLOCKS cap not reached. */
U128Q_INLINE int u128q_lane_cond_n86(
    u128q_u64 lo, u128q_u32 hi, u128q_u64 n_lo, u128q_u32 n_hi, u128q_u32 blocks) {
    return hi < U128Q_U96_HI_LIMIT && U128Q_U96_GE_N(hi, lo, n_hi, n_lo) &&
           blocks < U128Q_TAIL16_MAX_BLOCKS;
}
