
    /* Device-side count: the candidate count is the mask kernel's atomic
     * cursor, read here rather than by the host between the two kernels. On
     * overflow the claim range is empty (the host sees the flag afterwards and
     * rejects the batch), and the count is clamped to the buffer so no lane
     * reads past it. The host validates count, nonce and ids after the tail. */
    uint cand_count = (*dev_ovf != 0u) ? 0u : min(*dev_count, dev_cap);

    /* Stage the split sub-table (u32 pairs {3^f, c}, SPLIT_TAB_WORDS words)
     * into __local once per work-group, which for this persistent kernel is
     * once per launch. No lane has returned yet, so every lane reaches the
     * barrier. The 8-byte alignment lets each vload2 in u128q_split_stage
     * compile to one 64-bit shared load (ld.shared.v2.u32 on NVIDIA) instead
     * of two 32-bit loads. */
    __local uint split_tab[SPLIT_TAB_WORDS] __attribute__((aligned(8)));
    for (uint i = lid; i < SPLIT_TAB_WORDS; i += lsz) {
        split_tab[i] = split_ctab[i];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
#define SPLIT_TAB split_tab

    ulong cov = 0;
    ulong dig = 0;
    ulong blk = 0;
    uint cnt = 0;
    uint sc_claim = 0; /* self-check: claimed (processed + preflight-skipped) */
    uint sc_ids = 0;
    uint sc_nfl = 0;
    uint sc_fls = 0;
    u128q cmax;
    cmax.w[0] = 0;
    cmax.w[1] = 0;
    cmax.w[2] = 0;
    cmax.w[3] = 0;

    u128q pf;
    pf.w[0] = preflimit.x;
    pf.w[1] = preflimit.y;
    pf.w[2] = preflimit.z;
    pf.w[3] = preflimit.w;
    /* Host flag threshold: flag <=> ovf_item || cmax >= thr on the 16-step
     * path; exactness proof at u128q_flag_threshold16 (u128_limbs.h). */
    u128q thr;
    thr.w[0] = flag_thr.x;
    thr.w[1] = flag_thr.y;
    thr.w[2] = flag_thr.z;
    thr.w[3] = flag_thr.w;
    /* q(thr) = floor(thr / 2^54) as (hi:lo), loop-invariant (Q54_FLAG). */
    const ulong thr_q54_lo = (u128q_lo64(thr) >> 54) | (u128q_hi64(thr) << 10);
    const ulong thr_q54_hi = u128q_hi64(thr) >> 54;

    /* Slot loop. Slot state (per chain): packed id, n (n_lo, plus n_hi without
     * COLLATZ_GPU_TAIL_U64N; the lane loop needs n < 2^86), v = (hi:lo) < 2^86,
     * x = floor(block-start max / 2^54), block count and the setup overflow
     * bit. A lane claims `chunk` ids at a time from work_counter and hands them
     * to its slots in claim order; it stops claiming only when the cursor is
     * drained, and it consumes every id it claimed (counted into sc_claim and
     * sc_ids) before it claims again, so exactly one slot of one lane takes
     * each id in [0, cand_count).
     * A slot runs the u96 lane loop (u128q_split_step_maxhi) while
     * SLOT_COND holds; a chain that leaves it while still >= n
     * (v >= 2^86, or the block cap) finishes in u128q_tail16_resume, and one
     * that cannot start in it (v < n, v >= 2^86, n >= 2^86) runs
     * u128q_tail16_resume from the start. Either way the chain's blocks,
     * overflow bit and v_end equal u128q_tail16's and its max satisfies
     * Q54_FLAG (test_gpu_u128_limbs checks this), and SLOT_RETIRE folds it
     * exactly once. */
    /* Raw per-lane u128 coverage/digest accumulators, reduced mod 2^61-1 once
     * at fold-out (u128q_mod_mersenne61_u128): the same residue as two
     * Mersenne reductions per candidate, and the u128 cannot overflow
     * (< 2^32 candidates per lane, each term < 2^64, so the sum is < 2^96). */
    ulong raw_cov_lo = 0;
    ulong raw_cov_hi = 0;
    ulong raw_dig_lo = 0;
    ulong raw_dig_hi = 0;
    /* Q54_FLAG. Claim: with MX the folded max of a chain and T its true
     * block-start max, the test below flags every chain that the exact rule
     * `T >= thr` flags (the flagged set is a superset).
     * Proof. Let q(v) = floor(v / 2^54); q is monotone non-decreasing, so
     * q(max_i v_i) = max_i q(v_i). The lane loop keeps x = max over the
     * lane-loop block starts of q(v_i) = q(T_loop) (u128q_split_step_maxhi;
     * v < 2^86 there, so x < 2^32). SLOT_FIN rebuilds L0 = x * 2^54 <= T_loop
     * with q(L0) = x; a chain that resumes gets L = max(L0, R) from
     * u128q_tail16_resume, R the exact max of its later block starts, so
     * q(L) = max(q(T_loop), q(R)) = q(T); a chain that never enters the lane
     * loop has L = T. Hence q(MX) = q(T) and MX <= T always. If T >= thr then
     * q(T) >= q(thr), so q(MX) >= q(thr) and the chain is flagged. The overflow
     * bit flags unconditionally. The reported max stays exact because the host
     * re-walks every flagged candidate exactly (a superset of the needed ones),
     * and MX <= T keeps max_seen_bs and Rlb valid lower bounds of the global
     * max (they only steer the flag threshold). */
#define SLOT_FLAGTEST(MX, FL) (FL) = u128q_ge_q54((MX), thr_q54_lo, thr_q54_hi)
/* Add a raw u64 term into a (lo, hi) u128 accumulator with carry. */
#define RAWACC_ADD128(LO, HI, TERM)                                                            \
    do {                                                                                       \
        const ulong t__ = (TERM);                                                              \
        (LO) += t__;                                                                           \
        if ((LO) < t__) {                                                                      \
            ++(HI);                                                                            \
        }                                                                                      \
    } while (0)
#if COLLATZ_GPU_TAIL_U64N
#define SLOT_KEYS(N, BL, VE)                                                                   \
    do {                                                                                       \
        const ulong nl_ = u128q_lo64(N);                                                       \
        RAWACC_ADD128(raw_cov_lo, raw_cov_hi, u128q_mix64(u128q_cov_key_n64(nl_))); \
        RAWACC_ADD128(raw_dig_lo, raw_dig_hi, u128q_result_digest_term_n64((nl_), (ulong) (BL), (VE)));                      \
    } while (0)
#else
#define SLOT_KEYS(N, BL, VE)                                                                   \
    do {                                                                                       \
        RAWACC_ADD128(raw_cov_lo, raw_cov_hi, u128q_mix64(u128q_cov_key(N)));     \
        RAWACC_ADD128(raw_dig_lo, raw_dig_hi, u128q_result_digest_term((N), (ulong) (BL), (VE)));                            \
    } while (0)
#endif
    /* Fold one finished chain: blocks, count, max, coverage, digest and the
     * contender flag. A chain that left u128 or hit the block cap (OF) is only
     * flagged: the host folds it from its exact replay. */
#define SLOT_RETIRE(PK, N, BL, MX, VE, OF)                                                     \
    do {                                                                                       \
        const uint rbl_ = (BL);                                                                \
        const u128q rmx_ = (MX);                                                               \
        if ((OF) == 0u) {                                                                      \
            blk += (ulong) rbl_;                                                          \
            cnt += 1u;                                                                    \
            SLOT_KEYS(N, rbl_, VE);                                                            \
        }                                                                                      \
        if (u128q_compare_ge(rmx_, cmax) != 0) {                                           \
            cmax = rmx_;                                                                   \
        }                                                                                      \
        int fl_ = ((OF) != 0u) ? 1 : 0;                                                        \
        if (fl_ == 0) {                                                                        \
            SLOT_FLAGTEST(rmx_, fl_);                                                          \
        }                                                                                      \
        if (fl_ != 0) {                                                                        \
            sc_nfl += 1u;                                                                    \
            sc_fls += u128q_sc_term((PK), nonce);                                            \
            const uint idx_ = atomic_inc(flag_count);                                          \
            if (idx_ < flag_cap) {                                                             \
                flag_list[idx_] = (PK);                                                        \
            } else {                                                                           \
                *flag_ovf = 1u;                                                                \
            }                                                                                  \
        }                                                                                      \
    } while (0)
    /* n in the slot. With COLLATZ_GPU_TAIL_U64N every n < 2^64 and the slot
     * keeps n_lo only (u128q_lane_fast_ok, u128q_lane_cond). Without it every
     * n >= 2^64, and the slot also keeps n_hi = n.w[2]: the lane loop then
     * runs for n < 2^86 (u128q_lane_fast_ok_n86 and u128q_lane_cond_n86, whose
     * comment gives the bounds), and SLOT_GE_N, the "still >= n" test at the
     * lane exit, compares (hi, lo) with (n_hi, n_lo). */
#if COLLATZ_GPU_TAIL_U64N
#define SLOT_NHI_DECL(J)
#define SLOT_NHI_SET(J, N)
#define SLOT_FAST_OK(N, V) u128q_lane_fast_ok(N, V)
#define SLOT_COND(J) u128q_lane_cond(s##J##_lo, s##J##_hi, s##J##_nlo, s##J##_bl)
#define SLOT_N(J) u128q_from_halves(s##J##_nlo, 0)
#define SLOT_GE_N(J) (s##J##_hi != 0u || s##J##_lo >= s##J##_nlo)
#else
#define SLOT_NHI_DECL(J) uint s##J##_nhi = 0u;
#define SLOT_NHI_SET(J, N) s##J##_nhi = (N).w[2];
#define SLOT_FAST_OK(N, V) u128q_lane_fast_ok_n86(N, V)
#define SLOT_COND(J)                                                                           \
    u128q_lane_cond_n86(s##J##_lo, s##J##_hi, s##J##_nlo, s##J##_nhi, s##J##_bl)
#define SLOT_N(J) u128q_from_halves(s##J##_nlo, (ulong) s##J##_nhi)
#define SLOT_GE_N(J) U128Q_U96_GE_N(s##J##_hi, s##J##_lo, s##J##_nhi, s##J##_nlo)
#endif
    /* The slot keeps x = floor(block-start max / 2^54) (one u32, exact because
     * floor(./2^54) is monotone, so it commutes with max) instead of the 96-bit
     * max. SLOT_MAX_U128 rebuilds the lower bound x * 2^54, which has the same
     * floor(./2^54) as the true max; see Q54_FLAG. */
#define SLOT_DECL(J)                                                                           \
    uint s##J##_act = 0u;                                                                      \
    uint s##J##_pk = 0u;                                                                       \
    ulong s##J##_nlo = 0;                                                                      \
    SLOT_NHI_DECL(J)                                                                           \
    ulong s##J##_lo = 0;                                                                       \
    uint s##J##_hi = 0u;                                                                       \
    uint s##J##_mx = 0u;                                                                       \
    uint s##J##_bl = 0u;                                                                       \
    uint s##J##_of = 0u
#define SLOT_MAX_INIT(J) (s##J##_mx = u128q_x54(s##J##_lo, s##J##_hi))
#define SLOT_MAX_U128(J) u128q_from_x54(s##J##_mx)
    /* One u96 block jump with {3^f, c} composed from the split sub-table. */
#define SLOT_STEP(J)                                                                           \
    u128q_split_step_maxhi(SPLIT_TAB, &s##J##_lo, &s##J##_hi, &s##J##_mx, &s##J##_bl)
