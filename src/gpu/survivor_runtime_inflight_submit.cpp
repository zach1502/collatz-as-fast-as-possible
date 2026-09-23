#include "host_pipeline.hpp"
#include "survivor_runtime/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace collatz::gpu {

using namespace survivor_detail;

// Non-blocking readbacks of every word the host validates, into the
// pinned slot area; the last one carries the slot's event (in-order: when
// it has completed, every earlier read -- and the tail -- has too).
void SurvivorRunner::infl_enqueue_readbacks(InflightSlot& s, std::size_t groups) {
    const SlotLayout& L = s.lay;
    unsigned char* const h = s.host;
    const auto rd =
        [&](cl_mem buf, std::size_t bytes, std::size_t off, cl_event* ev, const char* w) {
            check(clEnqueueReadBuffer(queue_, buf, CL_FALSE, 0, bytes, h + off, 0, nullptr, ev), w);
        };
    // Flag-list prefix: the first flag_pref flag-list entries are read with the
    // slot readback, before the read that carries s.ev on the same in-order
    // queue, so s.ev complete means the prefix has landed. cpi_complete then
    // needs no blocking read when flag_count <= flag_pref; that read would wait
    // for the other in-flight batch. Entries past this batch's flag_count are
    // stale and never used, and reading device-only memory is harmless. Only
    // this slot's tail writes the flag list, and queue order completes it
    // before this read starts.
    s.flag_pref = flag_prefix_len(L.flags_n, s.flag_cap);
    rd(s.flaglist, s.flag_pref * sizeof(cl_uint), L.flags, nullptr, "slot read(flag prefix)");
    rd(s.candcnt, sizeof(cl_uint), L.candcnt, nullptr, "slot read(candcnt)");
    rd(s.candovf, sizeof(cl_uint), L.candovf, nullptr, "slot read(candovf)");
    rd(s.sc, 4u * sizeof(cl_uint), L.sc, nullptr, "slot read(sc)");
    rd(s.gcov, groups * sizeof(cl_ulong), L.gcov, nullptr, "slot read(gcov)");
    rd(s.gdig, groups * sizeof(cl_ulong), L.gdig, nullptr, "slot read(gdig)");
    rd(s.gblk, groups * sizeof(cl_ulong), L.gblk, nullptr, "slot read(gblk)");
    rd(s.gcnt, groups * sizeof(cl_uint), L.gcnt, nullptr, "slot read(gcnt)");
    rd(s.gmax, groups * sizeof(cl_uint4), L.gmax, nullptr, "slot read(gmax)");
    rd(s.gchk, groups * 4u * sizeof(cl_ulong), L.gchk, nullptr, "slot read(gchk)");
    rd(s.fc, sizeof(cl_uint), L.fc, nullptr, "slot read(flagcount)");
    rd(s.fo, sizeof(cl_uint), L.fo, &s.ev, "slot read(flagovf)");
}

// Two-deep run_cpi pipeline on the single in-order queue_. cpi_submit() stages the inputs and
// enqueues the mask, the persistent tail (which reads the candidate count on the device, so there
// is no host round trip between the two) and non-blocking readbacks of every word the host
// validates into a pinned per-slot host area, then returns the slot (0 or 1)
// without waiting. cpi_complete(slot) waits for that slot's last readback, reads
// the flag list (if any) on the side queue, and validates and reduces as run_cpi's
// device-count path does. At most 2 slots are busy; the caller completes the
// older one before submitting a third batch. cpi_submit returns -1, enqueuing
// nothing, when the batch is outside its scope (no candidates possible, or more
// than 2^(32-A) survivors), and the caller then uses run_cpi, which reports the
// latter as a candidate overflow. The folds are bit-identical to
// run_cpi over the same survivors for the same `rlb`; `rlb` only moves the flag threshold.
int SurvivorRunner::cpi_submit(const SurvivorBatch& batch,
                               const SurvivorConst& c,
                               collatz::u128 rlb,
                               std::uint32_t flag_cap,
                               std::size_t wg) {
    const std::size_t n = batch.size();
    if (n == 0 || n > packed_id_survivors(c.a_bits)) {
        return -1;
    }
    std::uint64_t cupper = 0;
    if (batch.cupper_two_a != 0 && batch.cupper_two_a == c.twoA) {
        // The producer already summed this exact a* vector. It
        // sets the hint only after the batch's final sort, and clear() and
        // verify_gpu's sub_job reset it. The value is identical, so cand_cap and
        // the mask self-check bound are too.
        cupper = batch.cupper_hint;
        ++pipe_.cupper_hints;
    } else {
        cupper = cand_upper_bound(
            batch.astar.begin(), batch.astar.begin() + static_cast<std::ptrdiff_t>(n), c.twoA);
    }
    std::size_t cand_cap = static_cast<std::size_t>(cupper);
    if (cand_cap > cand_id_cap()) {
        cand_cap = cand_id_cap();
        pipe_.cand_cap_ids = cand_cap;
    }
    if (cand_cap == 0) {
        return -1; // run_cpi returns an empty, checked result without the GPU
    }
    const int si = infl_.next;
    InflightSlot& s = infl_.slot[si];
    if (s.busy) {
        throw std::logic_error("cpi_submit with both in-flight slots busy");
    }
    const std::size_t groups = persist_groups();
    infl_ensure(s, groups, flag_cap);
    cpi_flag_cap_ = flag_cap;
    // Self-check nonce and test-injection draw. The nonce is fresh per dispatch,
    // as in sc_begin(). The injection sequence number is the batch's position in
    // validation order (consumed in cpi_complete, as a serial dispatch consumes
    // it when it validates), predicted here as the next number plus the batches
    // still ahead of this one. So every COLLATZ_GPU_SELFCHECK_INJECT=<first>,<count>
    // pattern means what it means serially ("3,4": batch 3 fails four times,
    // then falls back to the CPU). The prediction breaks only if a serial
    // dispatch validates in between; when an injection is armed, the caller
    // then discards and resubmits the later batch (cpi_discard).
    ++sc_nonce_;
    if (sc_nonce_ == 0) {
        sc_nonce_ = 1;
    }
    const cl_uint nonce = sc_nonce_;
    sc_cur_nonce_ = nonce;
    {
        const std::uint64_t ahead = infl_.slot[si ^ 1].busy ? 1u : 0u;
        const std::uint64_t seq = sc_seq_ + ahead;
        sc_inject_now_ = seq >= sc_inject_first_ && seq - sc_inject_first_ < sc_inject_count_;
        if (xfer_.inject_copy) {
            sc_inject_now_ = false; // the fault is a lost prefetch copy instead
        }
    }

    // A throw below drains the queues before any memory an enqueued command
    // reads (the batch vectors, the staging slots, this slot's area) unwinds.
    const DrainBothOnThrow drain_both(queue_, xfer_.copy_q);
    EventGuard pf_guard;
    cl_uint bits = 0u;
    const bool pf_hit = cpi_stage_inputs(batch, bits, pf_guard.e);
    xfer_.cur_bits = bits;
    // The candidate-id list is shared by both slots. Sound on the one in-order
    // queue_: mask(i+1) is enqueued after tail(i), and an in-order queue starts
    // a command only after every earlier command has completed, so tail(i) has
    // finished reading the ids before mask(i+1) writes any. (A regrow here
    // releases the old buffer; tail(i)'s bound handle keeps it alive.)
    ensure_cpi_capacity(cand_cap, 0);
    cpi_enqueue_mask(batch, c, nonce, cand_cap, bits, pf_guard.e, s.candcnt, s.candovf, s.sc);
    if (pf_guard.e != nullptr) {
        clReleaseEvent(pf_guard.e);
        pf_guard.e = nullptr;
    }
    static_assert(kSlotCheckRows == kInputCheckRows, "in-flight check rows");
    const SlotLayout& L = s.lay;
    s.chk_on = pf_hit;
    if (pf_hit) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* rows = reinterpret_cast<unsigned char (*)[16]>(s.host + L.chk);
        cpi_enqueue_input_checks(batch, bits, rows, s.chk_want, s.chk_len);
    }

    // The tail reads the candidate count on the device, so it is
    // queued straight behind the mask with no host round trip.
    PersistBufs pb;
    pb.fc = s.fc;
    pb.fo = s.fo;
    pb.wc = s.wc;
    pb.gcov = s.gcov;
    pb.gdig = s.gdig;
    pb.gblk = s.gblk;
    pb.gcnt = s.gcnt;
    pb.gmax = s.gmax;
    pb.gchk = s.gchk;
    pb.flaglist = s.flaglist;
    pb.dcount = s.candcnt;
    pb.dovf = s.candovf;
    // The INJECT_LOST hook, applied as in cpi_pass2_persistent.
    const bool inj_lost = sc_inject_now_ && sc_inject_lost_;
    if (inj_lost) {
        sc_inject_now_ = false;
    }
    persist_enqueue(
        c, rlb, flag_cap, wg, groups, pb, nonce, static_cast<cl_uint>(cand_cap), inj_lost);
    // The flush submits this batch, so the in-order queue holds it behind the
    // other slot's and the device never waits on the host between them.
    check(clFlush(queue_), "slot clFlush");

    infl_enqueue_readbacks(s, groups);
    // A still-armed self-check hook corrupts this batch's readback at completion.
    s.inject = sc_inject_now_;
    sc_inject_now_ = false;
    s.nonce = nonce;
    s.cupper = cupper;
    s.cand_cap = cand_cap;
    s.tail_groups = groups;
    s.mask_groups = static_cast<cl_uint>(n);
    s.flag_cap_used = flag_cap;
    s.busy = true;
    infl_.next = si ^ 1;
    ++infl_.batches;
    return si;
}

// Waits for a submitted slot and drops its result without validating it; the
// caller re-submits the batch later. Used only while a self-check injection is
// armed, so the injections stay in validation order.
void SurvivorRunner::cpi_discard(int si) {
    if (si < 0 || si > 1 || !infl_.slot[si].busy) {
        return;
    }
    InflightSlot& s = infl_.slot[si];
    // Drain the slot (its commands must finish before its buffers, the batch
    // vectors or the shared id list are reused) and drop the result unread.
    // No sequence number is consumed: the batch never validated.
    if (s.ev != nullptr) {
        check(clWaitForEvents(1, &s.ev), "slot wait(discard)");
        clReleaseEvent(s.ev);
        s.ev = nullptr;
    }
    s.busy = false;
    ++infl_.discarded;
}

} // namespace collatz::gpu
