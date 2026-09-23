#include "host_pipeline.hpp" // packed_id_survivors
#include "survivor_runtime/internal.hpp"

#include <cstdint>

namespace collatz::gpu {

using namespace survivor_detail;

// Candidate-per-item dispatch of one batch. The fused mask kernel rebuilds each
// survivor's Step-2 mask and scatters its surviving extensions into the packed
// candidate list with a work-group-local prefix sum and one atomic per group,
// so there is no host prefix sum and no mask round trip. The persistent tail
// then walks the candidates, with per-work-group reductions and a list of
// flagged candidates. `rlb` is the running lower bound on the global maximum
// excursion that sets the flag threshold. `flag_cap` bounds the flag list; on
// overflow `out.flag_overflow` is set and the caller falls back to run(). `wg`
// is the tail's work-group size. The candidate buffer is sized to the host
// upper bound sum(min(a_star, 2^A)); if that bound is capped and the true count
// exceeds the cap, `out.cand_overflow` is set too and the caller splits the
// batch. A batch of more than 2^(32-A) survivors, whose packed ids would wrap,
// is reported the same way without a dispatch. Throws OpenCLError on device
// failure. The folds are bit-identical to run() over the same survivors.
void SurvivorRunner::run_cpi(const SurvivorBatch& batch,
                             const SurvivorConst& c,
                             collatz::u128 rlb,
                             std::uint32_t flag_cap,
                             std::size_t wg,
                             CpiResult& out) {
    out = CpiResult{};
    const std::size_t n = batch.size();
    if (n == 0) {
        return;
    }
    if (n > packed_id_survivors(c.a_bits)) {
        out.flag_overflow = true;
        out.cand_overflow = true;
        return;
    }
    cpi_flag_cap_ = flag_cap;

    // Exact host upper bound on the candidate count: each survivor contributes
    // at most min(a_star, 2^A) candidates (the mask requires a < a_star and
    // a < 2^A), so the fused scatter can never write past this. Pre-size the
    // candidate buffer to it (clamped to the hard cap) -- no device round trip.
    std::uint64_t cupper = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t ai = batch.astar[i];
        cupper += ai < c.twoA ? ai : c.twoA;
    }
    std::size_t cand_cap = static_cast<std::size_t>(cupper);
    if (cand_cap > cand_id_cap()) {
        cand_cap = cand_id_cap();
        pipe_.cand_cap_ids = cand_cap;
    }
    if (cand_cap == 0) {
        out.sc_checked = true;
        return; // no possible candidates
    }
    const cl_uint nonce = sc_begin();

    // A throw below drains the compute and the copy queue before any staging or
    // caller memory a pending transfer reads is unwound.
    const DrainBothOnThrow drain_both(queue_, xfer_.copy_q);
    cl_uint bits = 0u;
    // Copy-queue event the mask must wait on. Owned from the swap below until
    // the mask is enqueued; a throw in between (any check()) releases it here.
    EventGuard pf_guard;
    cl_event& pf_wait = pf_guard.e;
    xfer_.chk_on = false;
    const bool pf_hit = cpi_stage_inputs(batch, bits, pf_wait);
    xfer_.cur_bits = bits;
    ensure_cpi_capacity(cand_cap, 0); // cand + control + flag buffers

    // Fused mask: Step-2 mask reconstruction + compaction.
    cpi_enqueue_mask(batch,
                     c,
                     nonce,
                     cand_cap,
                     bits,
                     pf_wait,
                     cpi_candcnt_buf_,
                     cpi_candovf_buf_,
                     sc_words_buf_);
    if (pf_wait != nullptr) {
        clReleaseEvent(pf_wait); // the mask command holds its own dependency
        pf_wait = nullptr;
    }
    if (pf_hit) {
        // The input-freshness reads follow the mask enqueue. They read the
        // active input set, which the mask only reads and a prefetch (into the
        // idle set) never writes, and the in-order queue still runs them after
        // the copy the mask waited on, so they see what the mask saw. Issued
        // before the mask, each read into pageable memory would be a
        // synchronous round trip on NVIDIA ahead of the mask launch.
        cpi_enqueue_input_checks(batch, bits, xfer_.chk, xfer_.chk_want, xfer_.chk_len);
        xfer_.chk_on = true;
    }
    // Device-side count: no host round trip. The tail reads the cursor and
    // overflow words on the device; the mask self-check words come back with
    // the tail's readbacks and are validated before any fold
    // (cpi_pass2_persistent). The C argument is only the sizing bound here.
    DevCount dc;
    dc.nonce = nonce;
    dc.cupper = cupper;
    dc.cand_cap = cand_cap;
    dc.groups = static_cast<cl_uint>(n);
    cpi_pass2_persistent(cand_cap, c, rlb, flag_cap, wg, out, dc);
    ++dc_batches_;
    // A consumed prefetch is validated here: its rows were read before the tail,
    // and the tail's last read blocked, so they have landed.
    if (prefetch_inputs_bad(out)) {
        return;
    }
    if (out.sc_checked && out.sc_ok && out.sc_tail_ids != dc.scw[0]) {
        sc_fail(out, "tail/mask candidate id sum");
    }
}

} // namespace collatz::gpu
