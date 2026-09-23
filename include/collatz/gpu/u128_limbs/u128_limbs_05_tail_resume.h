
/* The u96/limb block recurrence of u128q_tail16 resumed from a mid-walk
 * state: `blocks` block jumps are done, `maxv` is the running block-start max
 * and v the current block start (resume from the start with v == maxv,
 * blocks == 0). Each block takes the 96-bit step while v < 2^86 and n < 2^86,
 * else the 4-limb step, under the same guards and the same MAX_BLOCKS cap, so
 * the outputs equal u128q_tail16's for the whole chain. It reads the packed
 * TBL16: it runs only for the values the slot loop hands back (>= 2^86,
 * n >= 2^64 or the runaway cap), which decode to the same (3^f, c) as the
 * split sub-tables. */
U128Q_INLINE u128q_u32 u128q_tail16_resume(u128q n,
                                           u128q v,
                                           u128q maxv,
                                           u128q_u32 blocks,
                                           U128Q_GLOBAL const u128q_u32* tbl16,
                                           u128q* max_out,
                                           u128q_u32* overflow_out,
                                           u128q* vend_out) {
    u128q_u32 ovf = 0;
    const int n_ok = U128Q_U96_N_OK(n);
    const u128q_u32 n_hi = n.w[2];
    const u128q_u64 n_lo = u128q_lo64(n);
    while (u128q_compare_ge(v, n)) {
        if (blocks >= U128Q_TAIL16_MAX_BLOCKS) {
            ovf = 1; /* abort: treat a runaway item like a 2^128 overflow */
            break;
        }
        if (n_ok && v.w[3] == 0u && v.w[2] < U128Q_U96_HI_LIMIT) {
            u128q_u64 lo = u128q_lo64(v);
            u128q_u32 hi = v.w[2];
            u128q_u64 mlo = u128q_lo64(maxv);
            u128q_u64 mhi = u128q_hi64(maxv);
            while (hi < U128Q_U96_HI_LIMIT && U128Q_U96_GE_N(hi, lo, n_hi, n_lo) &&
                   blocks < U128Q_TAIL16_MAX_BLOCKS) {
                const u128q_u32 ee = tbl16[U128Q_CAST(u128q_u32, lo) & 0xFFFFu];
                const u128q_u64 k = U128Q_CAST(u128q_u64, u128q_pow3(ee & 0x1Fu));
                const u128q_u64 cc = U128Q_CAST(u128q_u64, ee >> 5);
                const u128q_u64 hi64 = U128Q_CAST(u128q_u64, hi);
                const int ge = (hi64 > mhi) || (hi64 == mhi && lo >= mlo);
                mlo = ge ? lo : mlo;
                mhi = ge ? hi64 : mhi;
                const u128q_u64 h_lo = (lo >> 16) | (hi64 << 48);
                const u128q_u64 p0 = (h_lo & 0xFFFFFFFFu) * k;
                const u128q_u64 p1 = (h_lo >> 32) * k;
                const u128q_u64 p2 = U128Q_CAST(u128q_u64, hi >> 16) * k;
                const u128q_u64 t = p0 + cc;
                const u128q_u64 nlo = t + (p1 << 32);
                const u128q_u64 carry = (nlo < t) ? 1u : 0u;
                hi = U128Q_CAST(u128q_u32, (p1 >> 32) + p2 + carry);
                lo = nlo;
                ++blocks;
            }
            v = u128q_from_halves(lo, U128Q_CAST(u128q_u64, hi));
            maxv = u128q_from_halves(mlo, mhi);
            continue; /* re-evaluate the outer condition (limb path if >= 2^86) */
        }
        {
            const u128q_u32 e = tbl16[v.w[0] & 0xFFFFu];
            const u128q h = u128q_shift_right_16(v);
            u128q_u32 o1 = 0;
            u128q_u32 o2 = 0;
            if (u128q_compare_ge(v, maxv)) {
                maxv = v;
            }
            const u128q prod = u128q_mul_u32(h, u128q_pow3(e & 0x1Fu), &o1);
            const u128q next = u128q_add_u32(prod, e >> 5, &o2);
            if (o1 != 0 || o2 != 0) {
                ovf = 1;
                break;
            }
            v = next;
            ++blocks;
        }
    }
    *max_out = maxv;
    *overflow_out = ovf;
    *vend_out = v;
    return blocks;
}
