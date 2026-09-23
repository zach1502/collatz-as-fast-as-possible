/* survivor_cpi.cl: GPU candidate-per-item (CPI) Step 2 and Step 3.
 *
 * survivor_tail16 runs one work-item per Step-1 survivor, and each item loops over
 * up to 2^A extensions whose Step-3 tails differ widely in block count, so a SIMD
 * group waits on its slowest lane. The kernels in this file schedule the tails
 * per candidate instead.
 *
 * The runtime prepends include/collatz/gpu/u128_limbs.h to this source, so the
 * u128q_* helpers are in scope. 128-bit values cross as uint4 limbs,
 * little-endian (.x, .y, .z, .w).
 *
 * cpi_mask_scatter_coalesced_wgscan (pass 1): one work-group per survivor
 * rebuilds the 2^A Step-2 candidate mask (bit-identical to Filter::mask and
 * survivor_tail16) and writes each surviving extension a as a packed candidate
 * id (s << a_bits | a) into cand_out, at slots that one atomic per work-group
 * reserves on cand_counter. No mask words touch global memory and the host does
 * no prefix sum. An id at or beyond cand_cap sets cand_ovf and is not written;
 * the host then splits the batch. Candidates land in arrival order; every
 * downstream fold is commutative, so the order does not change results.
 *
 * tail_persistent (pass 2): persistent lanes claim candidates from a global
 * cursor. Each decodes (s, a), forms n = n0 + a*2^M and m_a = m + 3^f*a, skips
 * n < preflimit, runs the shared u128q_tail16 block recurrence and reduces per
 * work-group in local memory (coverage and pow digest mod 2^61-1, block-count
 * sum, processed count, max block-start). Each work-group writes one partial of
 * each and the host reduces them; the folds are commutative, so any grouping
 * gives the same result. Max recovery uses a flag list: a candidate whose tail
 * bound (max_block_start * 3^16) >> 16 reaches the host lower bound Rlb, or
 * that overflowed, appends its packed id to flag_list through an atomic
 * counter. Past flag_cap the kernel sets flag_ovf and the host reruns the batch
 * on the per-survivor kernel. The host flags prefix-bound record contenders per
 * survivor, so they need no device max.
 */
/* The Step-2 bitvector table is one flat array of uint words, uploaded across
 * up to 4 device buffers (each holds a power-of-two run of levels) so that no
 * allocation exceeds CL_DEVICE_MAX_MEM_ALLOC_SIZE. Word W lives in buffer
 * W>>bv_lg at offset W&((1<<bv_lg)-1). A single-buffer run passes a bv_lg large
 * enough that every W maps to bv0. */
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

/* tail_persistent build option (host -D on the cpi program), result-neutral:
 *   COLLATZ_GPU_TAIL_U64N=1   candidate n as one ulong. The host sets it when
 *                             every n < 2^64.
 * Each 16-step block's {3^f, c} is composed from two 8-step lookups in one
 * 256-entry sub-table (u128q_split_kc in u128_limbs_04_split_tables.h), which
 * each work-group stages into 8-byte aligned __local once, instead of a row
 * of the 256 KiB __global TBL16. The block size stays 16, so every block's
 * (3^f, c), v, exit and fold are bit-identical to TBL16's. The host generates
 * the table image `split_ctab` (program-scope __constant, u32 pairs,
 * u128q_split_fill) and prepends it to this source. */
#ifndef COLLATZ_GPU_TAIL_U64N
#define COLLATZ_GPU_TAIL_U64N 0
#endif
/* Words of the split sub-table image: 256 u32 pairs. */
#define SPLIT_TAB_WORDS 512u

/* Narrowed per-survivor inputs (the upload trim), lossless. The f-dependent
 * fields (inv3f, bvbase and bvmode in the mask, 3^f in the tail) come from a
 * per-f table indexed by fidx_in[s] = f - fmin; they depend only on f. `trim`
 * is a per-batch bit set chosen by the host:
 *   bit 0: set (the f index).
 *   bit 1: n0_in holds one ulong per survivor (every n0 < 2^64).
 *   bit 2: m_in holds one ulong per survivor (the host checks every m < 2^64
 *          for the batch).
 * Without bit 1 or 2 the array holds uint4 limbs. `trim` is uniform over the
 * launch, so the branches are too. */
inline uint4 load_trim_u128(__global const uint4* p, uint s, uint narrow) {
    if (narrow != 0u) {
        const ulong v = ((__global const ulong*) p)[s];
        return (uint4) ((uint) v, (uint) (v >> 32), 0u, 0u);
    }
    return p[s];
}

/* Dropped-work-group self-check for the Step-2 mask kernel. The mask adds,
 * once per work-group (one global atomic), the survivors it covers into
 * sc_out[3], and the host requires the exact total. A group can fail to run
 * under GPU contention; that lowers the candidate count and the id sum
 * consistently, so only this word detects it.
 *
 * Test hook: the program is built with -DCOLLATZ_GPU_INJECT_DROPWG only when
 * COLLATZ_GPU_SELFCHECK_INJECT_DROPWG is set; then, on a dispatch where the
 * host wrote sc_out[4] != 0, the last work-group returns before doing anything
 * (a simulated dropped group). The condition is uniform over the group, so it
 * never splits a barrier. Without the define INJECT_DROP_WG is 0. */
#ifdef COLLATZ_GPU_INJECT_DROPWG
#define INJECT_DROP_WG(sc) ((sc)[4] != 0u && get_group_id(0) + 1u == get_num_groups(0))
#else
#define INJECT_DROP_WG(sc) 0
#endif
