
    loc_cov[lid] = cov;
    loc_blk[lid] = blk; /* ulong: exact for any per-lane candidate count */
    loc_cnt[lid] = cnt;
    loc_max[lid] = cmax;
    loc_dig[lid] = dig;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (uint stride = lsz >> 1; stride > 0u; stride >>= 1) {
        if (lid < stride) {
            loc_cov[lid] = u128q_mod_mersenne61(loc_cov[lid] + loc_cov[lid + stride]);
            loc_dig[lid] = u128q_mod_mersenne61(loc_dig[lid] + loc_dig[lid + stride]);
            loc_blk[lid] += loc_blk[lid + stride];
            loc_cnt[lid] += loc_cnt[lid + stride];
            if (u128q_compare_ge(loc_max[lid + stride], loc_max[lid]) != 0) {
                loc_max[lid] = loc_max[lid + stride];
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    const ulong r_cov = loc_cov[0];
    const ulong r_dig = loc_dig[0];
    const ulong r_blk = loc_blk[0];
    const uint r_cnt = loc_cnt[0];
    const u128q r_max = loc_max[0];
    if (lid == 0u) {
        const uint grp = get_group_id(0);
        gcov_out[grp] = r_cov;
        gblk_out[grp] = r_blk;
        gcnt_out[grp] = r_cnt;
        gmax_out[grp] = (uint4) (r_max.w[0], r_max.w[1], r_max.w[2], r_max.w[3]);
        gdig_out[grp] = r_dig;
    }
    /* Reduce the self-check words through the same __local arrays. */
    barrier(CLK_LOCAL_MEM_FENCE);
    loc_blk[lid] = ((ulong) sc_claim << 32) | (ulong) sc_nfl;
    loc_cov[lid] = (ulong) sc_ids;
    loc_dig[lid] = (ulong) sc_fls;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (uint stride = lsz >> 1; stride > 0u; stride >>= 1) {
        if (lid < stride) {
            loc_blk[lid] += loc_blk[lid + stride];
            loc_cov[lid] += loc_cov[lid + stride];
            loc_dig[lid] += loc_dig[lid + stride];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (lid == 0u) {
        const uint grp = get_group_id(0);
        const ulong w1 = loc_blk[0];
        const ulong w2 = (loc_dig[0] << 32) | (loc_cov[0] & 0xFFFFFFFFUL);
        gchk_out[4u * grp + 0u] = u128q_sc_group(nonce, r_cov, r_dig, r_blk, r_cnt, r_max, w1, w2);
        gchk_out[4u * grp + 1u] = w1;
        gchk_out[4u * grp + 2u] = w2;
        gchk_out[4u * grp + 3u] = (ulong) nonce;
    }
}
