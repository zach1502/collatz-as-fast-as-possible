#include "survivor_runtime/internal.hpp"

#include <cassert>
#include <cstring>

namespace collatz::gpu {

using namespace survivor_detail;

int SurvivorRunner::pack_staged(const SurvivorBatch& batch,
                                cl_uint& bits,
                                unsigned char* base,
                                StagedWrite* w) {
    // Fills one staging slot (`base`) with the batch's arrays, n0 and m
    // narrowed where the trim bits allow, and lists the writes. Destinations
    // are input-set members, resolved against the target set at enqueue time.
    return pack_staged_core(
        batch, bits, base, staged_full_bytes(batch.size()), w, xfer_.fmin, xfer_.fcount);
}

// The pack behind pack_staged, with no runner state, so a producer thread can
// run it. Returns the write count, or -1 when the layout the
// values allow does not fit in `cap` bytes.
int SurvivorRunner::pack_staged_core(const SurvivorBatch& batch,
                                     cl_uint& bits,
                                     unsigned char* base,
                                     std::size_t cap,
                                     StagedWrite* w,
                                     cl_uint fmin,
                                     [[maybe_unused]] std::size_t fc) {
    // Pure (reads only its arguments), so producer threads may run it on their
    // own block. A slot of staged_full_bytes(n) always fits; a smaller producer
    // block may not once a trim bit is dropped, and then this returns -1.
    // Bit 0 (the f index) is always set: upload_setup installs a per-f table
    // for every f in [fmin, M], and every survivor in a batch has such an f.
    assert((bits & 1u) != 0u);
    const std::size_t n = batch.size();
    int nw = 0;
    const auto add = [&](InputMember b, std::size_t bytes, const void* h, const char* what) {
        w[nw++] = StagedWrite{b, bytes, h, what};
    };
    for (;;) {
        nw = 0;
        std::size_t off = 0;
        const auto region = [&](std::size_t bytes) {
            unsigned char* p = base + off;
            off += staging_round_up(bytes);
            return p;
        };
        // Exact size of the layout `bits` selects (same order as below), so a
        // block that is too small is detected before anything is written.
        {
            const std::size_t w0 = (bits & 2u) != 0u ? sizeof(cl_ulong) : sizeof(cl_uint4);
            const std::size_t wm = (bits & 4u) != 0u ? sizeof(cl_ulong) : sizeof(cl_uint4);
            const std::size_t need = staging_round_up(n * w0) + staging_round_up(n * wm) +
                                     staging_round_up(n * sizeof(cl_uint)) + staging_round_up(n);
            if (need > cap) {
                return -1;
            }
        }
        cl_uint bad = 0u; // bits whose precondition some value violates
        // n0 / m: u64 when bit set (high limbs must be zero), else uint4.
        const auto put128 =
            [&](const std::vector<cl_uint4>& v, cl_uint bit, InputMember dst, const char* what) {
                if ((bits & bit) != 0u) {
                    unsigned char* p = region(n * sizeof(cl_ulong));
                    cl_uint hi = 0u;
                    for (std::size_t s = 0; s < n; ++s) {
                        hi |= v[s].s[2] | v[s].s[3];
                        const cl_ulong x = static_cast<cl_ulong>(v[s].s[0]) |
                                           (static_cast<cl_ulong>(v[s].s[1]) << 32);
                        std::memcpy(p + s * sizeof(cl_ulong), &x, sizeof(cl_ulong));
                    }
                    if (hi != 0u) {
                        bad |= bit;
                    }
                    add(dst, n * sizeof(cl_ulong), p, what);
                } else {
                    unsigned char* p = region(n * sizeof(cl_uint4));
                    std::memcpy(p, v.data(), n * sizeof(cl_uint4));
                    add(dst, n * sizeof(cl_uint4), p, what);
                }
            };
        const auto put32 = [&](const std::vector<cl_uint>& v, InputMember dst, const char* what) {
            unsigned char* p = region(n * sizeof(cl_uint));
            std::memcpy(p, v.data(), n * sizeof(cl_uint));
            add(dst, n * sizeof(cl_uint), p, what);
        };
        put128(batch.n0, 2u, &InputSet::n0, "xfer write(n0)");
        put128(batch.m, 4u, &InputSet::m, "xfer write(m)");
        put32(batch.astar, &InputSet::astar, "xfer write(astar)");
        {
            // f index into the per-run table.
            unsigned char* p = region(n);
            for (std::size_t s = 0; s < n; ++s) {
                const cl_uint fi = batch.f0[s] - fmin; // wraps below fmin
                assert(fi < fc);
                p[s] = static_cast<unsigned char>(fi);
            }
            add(&InputSet::fidx, n, p, "xfer write(fidx)");
        }
        if (bad == 0u) {
            break;
        }
        // Repack without the violated narrowing. Bits only ever clear, and
        // only bits 1 and 2 can be violated: terminates.
        bits &= ~bad;
    }
    return nw;
}

std::size_t SurvivorRunner::staged_full_bytes(std::size_t n) noexcept {
    // One slot holds the widest layout (n0 and m as uint4), so a trim bit
    // dropped mid-pack never needs a bigger slot.
    return 2 * staging_round_up(n * sizeof(cl_uint4)) + staging_round_up(n * sizeof(cl_uint)) +
           staging_round_up(n);
}

bool SurvivorRunner::prepack_matches(const SurvivorBatch& batch) const noexcept {
    const PrePack& pp = xfer_.prepack;
    const std::size_t n = batch.size();
    return pp.valid && n != 0 && pp.ptr == batch.n0.data() && pp.n == n &&
           std::memcmp(&pp.first, &batch.n0[0], sizeof(cl_uint4)) == 0 &&
           std::memcmp(&pp.last, &batch.n0[n - 1], sizeof(cl_uint4)) == 0;
}

// Does prefetch_inputs' host work now (waits for a free staging slot and packs
// the narrowed arrays into it), so a later prefetch_inputs on the same batch
// object only enqueues the copy. If another upload takes the slot first, the
// pre-pack is dropped and redone at prefetch time. Enqueues no device command.
void SurvivorRunner::prefetch_prepack(const SurvivorBatch& batch) {
    // The host half of prefetch_inputs, run while the device still has two
    // batches queued (before the dispatch thread waits on the older one), so
    // the later prefetch_inputs only enqueues. Same scope as
    // prefetch_inputs.
    if (xfer_.copy_q == nullptr || xfer_.pf_pending || batch.size() == 0) {
        return;
    }
    if (batch.pp_block >= 0) {
        return; // a producer already packed it; prefetch_inputs only enqueues
    }
    cl_uint bits = trim_bits_for(batch);
    const std::size_t n = batch.size();
    const int slot = xfer_.next_stage;
    if (const cl_event busy = xfer_.stage[slot].ev; busy != nullptr) {
        // Performance only: the pre-pack exists to take host work off the
        // critical path, so never block on the slot's last write here. If it
        // has not completed, skip; prefetch_inputs packs the batch as usual.
        cl_int es = CL_SUCCESS;
        const cl_int q =
            clGetEventInfo(busy, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(es), &es, nullptr);
        if (q != CL_SUCCESS || es != CL_COMPLETE) {
            ++pipe_.prepack_busy;
            return;
        }
    }
    StagingSlot& st = next_staging(staged_full_bytes(n)); // slot's last write already complete
    PrePack& pp = xfer_.prepack;
    pp.nw = pack_staged(batch, bits, st.host, pp.w);
    pp.bits = bits;
    pp.slot = slot;
    pp.ptr = batch.n0.data();
    pp.n = n;
    pp.first = batch.n0[0];
    pp.last = batch.n0[n - 1];
    pp.valid = true;
    ++pipe_.prepacks;
}

} // namespace collatz::gpu
