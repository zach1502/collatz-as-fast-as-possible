    /* Fill an empty slot: consume claimed ids until one starts in the lane
     * loop (preflight skips and slow-path chains are finished here). */
#define SLOT_REFILL(J)                                                                         \
    while (s##J##_act == 0u && drained == 0) {                                                 \
        if (gid >= gend) {                                                                     \
            const uint base_ = atomic_add(work_counter, chunk);                                                   \
            if (base_ >= cand_count) {                                                         \
                drained = 1;                                                                   \
                break;                                                                         \
            }                                                                                  \
            gid = base_;                                                                       \
            gend = base_ + chunk;                                                         \
            if (gend > cand_count) {                                                           \
                gend = cand_count;                                                             \
            }                                                                                  \
        }                                                                                      \
        const uint pk_ = cand_in[gid];                                                     \
        ++gid;                                                                                 \
        sc_claim += 1u;                                                                      \
        sc_ids += u128q_sc_term(pk_, nonce);                                                 \
        u128q n_;                                                                              \
        u128q v_;                                                                              \
        uint of_ = 0u;                                                                         \
        if (slot_setup(pk_, n0_in, m_in, a_bits, twoA_mask, m_shift, pf, trim,                 \
                       fidx_in, p3ftab, &rc_s, &rc_n0, &rc_m, &rc_p3, &n_, &v_, &of_) == 0) {  \
            continue;                                                                          \
        }                                                                                      \
        if (SLOT_FAST_OK(n_, v_)) {                                                            \
            s##J##_pk = pk_;                                                                   \
            s##J##_nlo = u128q_lo64(n_);                                                       \
            SLOT_NHI_SET(J, n_)                                                                \
            s##J##_lo = u128q_lo64(v_);                                                        \
            s##J##_hi = v_.w[2];                                                               \
            SLOT_MAX_INIT(J);                                                                  \
            s##J##_bl = 0u;                                                                    \
            s##J##_of = of_;                                                                   \
            s##J##_act = 1u;                                                                   \
        } else {                                                                               \
            u128q mx_;                                                                         \
            u128q ve_;                                                                         \
            uint ov_ = 0u;                                                                     \
            const uint bl_ = u128q_tail16_resume(n_, v_, v_, 0u, tbl16, &mx_, &ov_, &ve_);     \
            SLOT_RETIRE(pk_, n_, bl_, mx_, ve_, of_ | ov_);                                    \
        }                                                                                      \
    }
    /* Retire a slot that left the lane loop (finishing it first if >= n). */
#define SLOT_FIN(J)                                                                            \
    if (s##J##_act != 0u && !SLOT_COND(J)) {                                                   \
        u128q mx_ = SLOT_MAX_U128(J);                                                          \
        u128q ve_ = u128q_from_halves(s##J##_lo, (ulong) s##J##_hi);                           \
        const u128q n_ = SLOT_N(J);                                                            \
        uint bl_ = s##J##_bl;                                                                  \
        uint ov_ = 0u;                                                                         \
        if (SLOT_GE_N(J)) {                                                                    \
            bl_ = u128q_tail16_resume(n_, ve_, mx_, bl_, tbl16, &mx_, &ov_, &ve_);             \
        }                                                                                      \
        SLOT_RETIRE(s##J##_pk, n_, bl_, mx_, ve_, s##J##_of | ov_);                            \
        s##J##_act = 0u;                                                                       \
    }

    /* Survivor reuse cache (see slot_setup). */
    uint rc_s = 0xFFFFFFFFu;
    u128q rc_n0 = cmax;
    u128q rc_m = cmax;
    u128q rc_p3 = cmax;
    uint gid = 0u;
    uint gend = 0u;
    int drained = 0;
    SLOT_DECL(0);
    for (;;) {
        SLOT_REFILL(0)
        if (s0_act == 0u) {
            break;
        }
        /* The round's step-loop iteration count. Nothing reads it; the
         * compiled loop differs without it, so the kernel keeps it. */
        uint ks_it = 0u;
        while (SLOT_COND(0)) {
            SLOT_STEP(0);
            ++ks_it;
        }
        (void) ks_it;
        SLOT_FIN(0)
    }
    /* Reduce the raw per-lane sums mod 2^61-1 once (the same residue as the
     * per-term fold; see u128q_mod_mersenne61_u128). */
    cov = u128q_mod_mersenne61_u128(raw_cov_lo, raw_cov_hi);
    dig = u128q_mod_mersenne61_u128(raw_dig_lo, raw_dig_hi);
#undef RAWACC_ADD128
#undef SLOT_FLAGTEST
#undef SLOT_KEYS
#undef SLOT_RETIRE
#undef SLOT_DECL
#undef SLOT_COND
#undef SLOT_STEP
#undef SPLIT_TAB
#undef SLOT_REFILL
#undef SLOT_MAX_INIT
#undef SLOT_MAX_U128
#undef SLOT_FIN
#undef SLOT_NHI_DECL
#undef SLOT_NHI_SET
#undef SLOT_FAST_OK
#undef SLOT_N
#undef SLOT_GE_N
