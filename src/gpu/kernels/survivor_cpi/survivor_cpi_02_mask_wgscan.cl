
/* cpi_mask_scatter_coalesced_wgscan: the device mask, one work-group of
 * L = 2^A/32 lanes per survivor. The prologue (inputs, one coalesced window
 * load, straddle exchange, mod-9 and a_star filters) gives each lane its
 * `live` bits. The reservation is aggregated per work-group: each live lane
 * takes its offset inside the survivor's run with one __local atomic_add of
 * popcount(live) and adds its sum of sc_term to a second __local word; 
 * then lane 0 alone issues one atomic_add(cand_counter, total) plus the two 
 * sc_out adds and broadcasts the base. Lane l writes its ids at base + its
 * offset + j, so the order inside a run is the lanes' arrival order.
 *
 * Each survivor's ids form one contiguous run inside cand_out, and every
 * downstream fold is order-independent. cand_counter advances by the true
 * total, so the host's count and overflow split see the true value, and an id
 * at or past cand_cap sets cand_ovf and is not written (the per-id
 * `idx < cand_cap` rule). The self-check words receive the
 * {sum sc_term(id), nonce, count} sums (u32 addition is commutative), and
 * sc_out[3] counts completed work-groups (the host checks it == count).
 *
 * The host must dispatch local size L = 2^A/32 and pass 5L + 2 uints of
 * __local scratch: words[0..L] straddle, the work-group's candidate count at
 * word L+1, its id sum at word 3L+1 and its base at word 5L+1. */
__kernel void cpi_mask_scatter_coalesced_wgscan(__global const uint* bv0,      /* 0 */
                                                __global const uint4* n0_in,   /* 1 */
                                                __global const uint4* m_in,    /* 2 */
                                                __global const uint* astar_in, /* 3 */
                                                uint count,                     /* 4 */
                                                uint b_mask,                    /* 5 */
                                                uint pow2m_mod9,                /* 6 */
                                                uint a_bits,                    /* 7 */
                                                uint cand_cap,                  /* 8 */
                                                __global uint* cand_out,        /* 9 */
                                                __global volatile uint* cand_counter, /* 10 */
                                                __global uint* cand_ovf,        /* 11 */
                                                __local uint* words, /* 12: 5L+2 uints */
                                                __global const uint* bv1,       /* 13 */
                                                __global const uint* bv2,       /* 14 */
                                                __global const uint* bv3,       /* 15 */
                                                uint bv_lg,                     /* 16 */
                                                uint nonce,                     /* 17: self-check */
                                                __global volatile uint* sc_out, /* 18: self-check */
        uint trim,                       /* 19: narrowed-input bits */
        __global const uchar* fidx_in,   /* 20: f - fmin per survivor */
        __global const uint4* ftab       /* 21: {inv3f, bvbase, bvmode, 0} per f - fmin */
        ) {
    /* Global group index, so the host may slice the launch with a global
     * work offset (a multiple of L); equals get_group_id(0) when unsliced. */
    const uint s = (uint) (get_global_offset(0) / get_local_size(0)) + (uint) get_group_id(0);
    const uint l = get_local_id(0);      /* lane / window word index */
    const uint L = (uint) get_local_size(0); /* = 2^A/32 */
    /* Uniform across the group (host sizes global = count*L); every lane below
     * reaches every barrier. */
    if (s >= count || INJECT_DROP_WG(sc_out)) {
        return; /* INJECT_DROP_WG: test hook, a simulated dropped work-group */
    }
    if (s == 0u && l == 0u) {
        sc_out[1] = nonce;
    }
    uint4 n0v = load_trim_u128(n0_in, s, trim & 2u);
    uint4 mv = load_trim_u128(m_in, s, trim & 4u);
    u128q n0;
    u128q m;
    n0.w[0] = n0v.x;
    n0.w[1] = n0v.y;
    n0.w[2] = n0v.z;
    n0.w[3] = n0v.w;
    m.w[0] = mv.x;
    m.w[1] = mv.y;
    m.w[2] = mv.z;
    m.w[3] = mv.w;

    const uint astar = astar_in[s];
    ulong inv3f;
    uint bvbase;
    uint bvmode;
    const uint4 ft = ftab[fidx_in[s]];
    inv3f = (ulong) ft.x;
    bvbase = ft.y;
    bvmode = ft.z;

    const ulong m_res = u128q_lo64(m) & (ulong) b_mask;
    const ulong j0 = (m_res * inv3f) & (ulong) b_mask;
    const uint r0 = u128q_mod9(n0);

    const uint s0 = (uint) (j0 & 31u);
    const uint base = (uint) (j0 >> 5);
    const uint wmask = b_mask >> 5; /* 2^B/32 - 1 */

    if (bvmode != 0u) {
        words[l] = 0xFFFFFFFFu;
        if (l == L - 1u) {
            words[l + 1u] = 0xFFFFFFFFu;
        }
    } else {
        words[l] = bv_word(bv0, bv1, bv2, bv3, bv_lg, bvbase + ((base + l) & wmask));
        if (l == L - 1u) {
            words[l + 1u] = bv_word(bv0, bv1, bv2, bv3, bv_lg, bvbase + ((base + l + 1u) & wmask));
        }
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    const uint w_lo = words[l];
    const uint w_hi = words[l + 1u];
    const uint chunk = (s0 == 0u) ? w_lo : ((w_lo >> s0) | (w_hi << (32u - s0)));

    const uint sid_hi = s << a_bits;
    const uint a_base = l << 5;
    /* One __constant lookup gives the lane's mod-9 pattern word (row
     * p = pow2m_mod9, column r = start residue). */
    const uint pat = u128q_mod9_pat[pow2m_mod9 * 9u + (r0 + a_base * pow2m_mod9) % 9u];
    const uint amask = (astar <= a_base) ? 0u
                       : (astar - a_base >= 32u) ? 0xFFFFFFFFu
                                                 : ((1u << (astar - a_base)) - 1u);
    const uint live = chunk & pat & amask;
    const uint cnt = popcount(live);
    uint ids = 0u;
    for (uint rest = live; rest != 0u; rest &= rest - 1u) {
        const uint k = 31u - clz(rest & (uint) (-(int) rest)); /* ctz */
        ids += u128q_sc_term(sid_hi | (a_base + k), nonce);
    }

    __local uint* pc = words + L + 1u; /* pc[0]: the work-group's candidate count */
    __local uint* ps = pc + 2u * L;    /* ps[0]: the work-group's id sum */
    __local uint* wg_base = ps + 2u * L;
    uint excl; /* this lane's exclusive offset inside the survivor's run */
    /* Each live lane takes its offset with a __local atomic_add, three
     * barriers in all. */
    if (l == 0u) {
        pc[0] = 0u;
        ps[0] = 0u;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    excl = 0u;
    if (cnt != 0u) {
        excl = atomic_add(&pc[0], cnt);
        atomic_add(&ps[0], ids);
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    if (l == 0u) {
        const uint total = pc[0];
        uint b0 = 0u;
        if (total != 0u) {
            b0 = atomic_add(cand_counter, total);
            atomic_add(&sc_out[0], ps[0]);
            atomic_add(&sc_out[2], total);
        }
        /* Self-check: one completed work-group (the host expects `count`).
         * Only this count detects a dropped group. */
        atomic_inc(&sc_out[3]);
        *wg_base = b0;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    if (live == 0u) {
        return; /* no barrier follows */
    }
    uint idx = *wg_base + excl;
    for (uint rest = live; rest != 0u; rest &= rest - 1u) {
        const uint k = 31u - clz(rest & (uint) (-(int) rest)); /* ctz */
        if (idx < cand_cap) {
            cand_out[idx] = sid_hi | (a_base + k);
        } else {
            *cand_ovf = 1u; /* host falls back for the whole batch */
        }
        ++idx;
    }
}
