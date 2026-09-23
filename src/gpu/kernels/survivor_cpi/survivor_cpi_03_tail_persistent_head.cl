
/* Per-candidate setup of the slot loop: n = n0 + a * 2^M, the preflight test
 * n >= preflimit, the seed m_a = m + 3^f * a, and the o1/o2 overflow bits
 * folded into *of_out. Returns 0 for a preflight-skipped candidate. rc_* is
 * the survivor reuse cache: it holds the inputs of survivor *rc_s, which are
 * read-only for the dispatch. */
inline int slot_setup(uint packed,
                      __global const uint4* n0_in,
                      __global const uint4* m_in,
                      uint a_bits,
                      uint twoA_mask,
                      uint m_shift,
                      u128q pf,
                      uint trim,
                      __global const uchar* fidx_in,
                      __global const uint4* p3ftab,
                      uint* rc_s,
                      u128q* rc_n0,
                      u128q* rc_m,
                      u128q* rc_p3,
                      u128q* n_out,
                      u128q* v_out,
                      uint* of_out) {
    const uint s = packed >> a_bits;
    const uint a = packed & twoA_mask;
    u128q n0;
    u128q m;
    u128q p3f;
    if (s != *rc_s) {
        *rc_s = s;
        const uint4 n0v = load_trim_u128(n0_in, s, trim & 2u);
        const uint4 mv = load_trim_u128(m_in, s, trim & 4u);
        const uint4 p3v = p3ftab[fidx_in[s]];
        rc_n0->w[0] = n0v.x;
        rc_n0->w[1] = n0v.y;
        rc_n0->w[2] = n0v.z;
        rc_n0->w[3] = n0v.w;
        rc_m->w[0] = mv.x;
        rc_m->w[1] = mv.y;
        rc_m->w[2] = mv.z;
        rc_m->w[3] = mv.w;
        rc_p3->w[0] = p3v.x;
        rc_p3->w[1] = p3v.y;
        rc_p3->w[2] = p3v.z;
        rc_p3->w[3] = p3v.w;
    }
    n0 = *rc_n0;
    m = *rc_m;
    p3f = *rc_p3;
#if COLLATZ_GPU_TAIL_U64N
    /* n < 2^N <= 2^64 (host-checked), so n0 < 2^64, a * 2^M < 2^64 and the
     * sum does not wrap: n is lo64(n0) + (a << M) exactly. A preflimit
     * >= 2^64 exceeds every such n, so the preflight compare is exact. */
    const ulong n_lo = u128q_lo64(n0) + ((ulong) a << m_shift);
    if ((pf.w[2] | pf.w[3]) != 0u || n_lo < u128q_lo64(pf)) {
        return 0;
    }
    const u128q n = u128q_from_halves(n_lo, 0);
#else
    const u128q n = u128q_add_shl_u32(n0, a, m_shift);
    if (u128q_compare_ge(n, pf) == 0) {
        return 0; /* n < preflimit: skipped, the preflight covers it */
    }
#endif
    uint o1 = 0;
    const u128q prod = u128q_mul_u32(p3f, a, &o1);
    uint o2 = 0;
    const u128q m_a = u128q_add(m, prod, &o2);
    *n_out = n;
    *v_out = m_a;
    *of_out = (o1 != 0u || o2 != 0u) ? 1u : 0u;
    return 1;
}

/* tail_persistent: persistent-lane Step-3 tail. tail_persistent launches a
 * fixed grid (a few times compute units x work-group size) of lanes that each
 * loop: take the next id of a claimed chunk, walk its chain, and claim the
 * next chunk of candidates from a global work cursor when the chunk is used
 * up. The cursor balances the work across SIMD groups and work-groups: a group
 * that draws short tails claims more chunks. Inside a SIMD group the lanes run
 * the block loop in lockstep, so a lane whose chain ends first idles until the
 * longest chain of the round ends; then every lane retires its chain and takes
 * its next id together (SLOT_FIN, SLOT_REFILL). No lane takes new work while
 * its neighbours are still walking. Every lane keeps its own (v, n, blocks)
 * state; the claim branch is the only extra control flow, and the exit
 * condition (base >= cand_count) is evaluated per lane with no barrier in the
 * loop, so no barrier is reached in divergence.
 *
 * Each lane accumulates its folds in registers across every candidate it
 * processes (coverage and pow digest mod 2^61-1, block sum, count, max
 * block-start) and flags record contenders into the flag list. After the loop
 * every lane joins one per-work-group tree reduction (the exit is per lane, not
 * per group, but every lane exits once the cursor is drained), and each group
 * writes one partial of each fold. Every fold is commutative, so the result is
 * the same for any claim order.
 *
 * work_counter: one global uint; atomic_add(chunk) claims [base, base+chunk).
 * The host resets it to 0 before each dispatch. chunk amortizes the global
 * atomic over `chunk` candidates. */
__kernel void tail_persistent(__global const uint* tbl16,         /* 0 */
                              __global const uint* cand_in,        /* 1 */
                              __global const uint4* n0_in,         /* 2 */
                              __global const uint4* m_in,          /* 3 */
                              uint a_bits,                         /* 4 */
                              uint twoA_mask,                      /* 5 */
                              uint m_shift,                        /* 6 */
                              uint4 preflimit,                     /* 7 */
                              uint flag_cap,                       /* 8 */
                              __global ulong* gcov_out,            /* 9 */
                              __global ulong* gblk_out,            /* 10 */
                              __global uint* gcnt_out,             /* 11 */
                              __global uint4* gmax_out,            /* 12 */
                              __global volatile uint* flag_count,  /* 13 */
                              __global uint* flag_list,            /* 14 */
                              __global uint* flag_ovf,             /* 15 */
                              __local ulong* loc_cov,              /* 16 */
                              __local ulong* loc_blk,              /* 17 */
                              __local uint* loc_cnt,               /* 18 */
                              __local u128q* loc_max,              /* 19 */
                              __global ulong* gdig_out,            /* 20 */
                              __local ulong* loc_dig,              /* 21 */
                              uint chunk,                          /* 22 */
                              __global volatile uint* work_counter, /* 23 */
                              uint nonce,                     /* 24: self-check dispatch nonce */
                              __global ulong* gchk_out,       /* 25: self-check, 4 ulongs per group */
                              __global const uint* dev_count, /* 26: device-mask candidate cursor */
                              __global const uint* dev_ovf,   /* 27: device-mask overflow flag */
                              uint dev_cap,                   /* 28: candidate buffer capacity */
                              uint trim,                      /* 29: narrowed-input bits */
                              __global const uchar* fidx_in,  /* 30: f - fmin per survivor */
                              __global const uint4* p3ftab,   /* 31: 3^f per f - fmin */
                              uint4 flag_thr /* 32: host threshold u128q_flag_threshold16(Rlb) */
) {
    const uint lid = get_local_id(0);
    const uint lsz = get_local_size(0);
