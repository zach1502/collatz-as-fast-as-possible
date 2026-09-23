#include "collatz/step2_cache.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

void GpuRun::finish() {
    fill_result();
    print_run_stats();
}

void GpuRun::fill_result() {
    (void) kCoveragePrime;
    res.candidates_checked = candidates;
    res.step2_masked_out = masked_out;
    res.coverage_checksum = coverage;
    res.pow_checksum = pow_digest;
    res.blocks_total = blocks_total;      // diagnostic block-count sum
    res.pow_available = true;             // block digest via the shared table16 recurrence.
    res.pow_kernel_id = 16;               // table16 block digest.
    res.step3_steps_total = blocks_total; // table16 mirrors the block-count sum.
    res.max_excursion = rec.maxv;
    res.max_excursion_n = rec.maxn;
    res.max_exceeds_u128 = rec.exceeds;
    // An excursion above 2^128-1 beats every u128 record (saturated to u128_max
    // for it), so the exact wide record decides max and argmax (verify::run).
    if (!rec.wide_v.empty()) {
        res.max_excursion_wide = rec.wide_v;
        res.max_excursion_n = rec.wide_n;
        res.max_excursion = collatz::u128_max;
        res.max_exceeds_u128 = true;
    }
    res.overflow_fallbacks = rec.overflow;
    if (rl.on) {
        res.records = collatz::verify::finish_records(std::move(rec.hits),
                                                      std::move(rec.wide_hits),
                                                      rl.cap,
                                                      rec.hits_dropped,
                                                      res.records_truncated);
    }
    // Exact-max soundness guard, shared with the CPU path and block-aware.
    res.max_exact = collatz::verify::result_max_exact(
        opts.N, opts.block_h, collatz::verify::unwalked_depth(opts), res);
    res.prefix_walks = 0; // perf diagnostic; not defined for the GPU path.
    res.step1 = st;
    res.seconds_step1_2 = filter_seconds + st.seconds;
    res.seconds_total = std::chrono::duration<double>(Clock::now() - t_start).count();
    res.gpu_selfcheck_on = true;
    res.gpu_selfcheck_batches = sc_batches;
    res.gpu_selfcheck_retried = sc_retried;
    res.gpu_selfcheck_cpu_fallback = sc_cpu_fallback;
    res.gpu_selfcheck_unchecked = sc_unchecked;
}

// The stderr summaries (selfcheck, transfer, pipeline, producer and upload
// lines) and the Result counters they report.
void GpuRun::print_run_stats() {
    std::fprintf(stderr,
                 "gpu selfcheck: batches=%llu retried=%llu cpu_fallback=%llu%s\n",
                 static_cast<unsigned long long>(sc_batches),
                 static_cast<unsigned long long>(sc_retried),
                 static_cast<unsigned long long>(sc_cpu_fallback),
                 sc_unchecked != 0 ? (" unchecked=" + std::to_string(sc_unchecked)).c_str() : "");
    {
        // Transfer path, on stderr only (not in stdout or --json).
        std::fprintf(stderr,
                     "gpu transfer: pinned=%d copy_queue=%d upload_trim=%d "
                     "prefetch issued=%llu hits=%llu input_check_fail=%llu trimmed_uploads=%llu "
                     "(m_u64=%llu)\n",
                     runner.pinned_enabled() ? 1 : 0,
                     runner.copy_queue_ready() ? 1 : 0,
                     runner.upload_trim_enabled() ? 1 : 0,
                     static_cast<unsigned long long>(runner.prefetch_issued()),
                     static_cast<unsigned long long>(runner.prefetch_hits()),
                     static_cast<unsigned long long>(runner.input_check_failures()),
                     static_cast<unsigned long long>(runner.trimmed_batches()),
                     static_cast<unsigned long long>(runner.trimmed_m64()));
    }

    {
        // Host pipeline, on stderr only (not in stdout or --json).
        const SurvivorRunner::PipelineStats& e = runner.pipeline_stats();
        std::fprintf(stderr,
                     "gpu pipeline: prefetch_early issued=%llu marker_waited=%llu "
                     "flagread_k=%zu (prefix_only=%llu second_read=%llu) "
                     "dispatch_offload (cupper_hints=%llu prepacks=%llu hits=%llu lost=%llu "
                     "busy=%llu) split_pipelined=%llu cand_cap_ids=%llu\n",
                     static_cast<unsigned long long>(e.pf_early_n),
                     static_cast<unsigned long long>(e.pf_early_wait),
                     e.flag_k,
                     static_cast<unsigned long long>(e.flag_prefix_ok),
                     static_cast<unsigned long long>(e.flag_rest_rd),
                     static_cast<unsigned long long>(e.cupper_hints),
                     static_cast<unsigned long long>(e.prepacks),
                     static_cast<unsigned long long>(e.prepack_hits),
                     static_cast<unsigned long long>(e.prepack_lost),
                     static_cast<unsigned long long>(e.prepack_busy),
                     static_cast<unsigned long long>(split_pipelined_halves),
                     static_cast<unsigned long long>(e.cand_cap_ids));
        // The split and id-cap counters also go on the Result.
        auto& d = res.gpu_pipeline;
        d.cpi_split_batches = cpi_split_batches;
        d.split_pipelined = split_pipelined_halves;
        d.cand_cap_ids = e.cand_cap_ids;
    }
    {
        // Producer-side pack, on stderr only (not in stdout or --json).
        const SurvivorRunner::PackPoolStats e = runner.pack_pool_stats();
        std::fprintf(stderr,
                     "gpu producer: producer_prep=%d (blocks=%zu x %.1f MiB pinned=%d packed=%llu "
                     "hits=%llu nopool=%llu nofit=%llu mismatch=%llu stale=%llu unused=%llu "
                     "pack_thread_s=%.3f)\n",
                     producer_prep ? 1 : 0,
                     e.blocks,
                     static_cast<double>(e.block_bytes) / (1024.0 * 1024.0),
                     e.pinned ? 1 : 0,
                     static_cast<unsigned long long>(e.packed),
                     static_cast<unsigned long long>(e.hits),
                     static_cast<unsigned long long>(e.nopool),
                     static_cast<unsigned long long>(e.nofit),
                     static_cast<unsigned long long>(e.mismatch),
                     static_cast<unsigned long long>(e.stale),
                     static_cast<unsigned long long>(e.unused),
                     producer_pack_s.load());
    }
    {
        // The staged bitvector upload, printed on the stderr `gpu upload:` line
        // with the device-flag count (a superset of the exact rule, Q54_FLAG in
        // survivor_cpi.cl).
        auto& d = res.gpu_upload;
        d.early_upload = upload_chunk_count != 0;
        d.upload_chunks = upload_chunk_count;
        std::fprintf(stderr,
                     "gpu upload: early_upload=%d chunks=%zu upload_s=%.3f tail_flagged=%llu\n",
                     upload_chunk_count != 0 ? 1 : 0,
                     upload_chunk_count,
                     t_upload_s,
                     static_cast<unsigned long long>(cpi_tail_flagged));
    }
}

std::string GpuRun::summary_message() const {
    return "gpu verify: kernel=" + std::to_string(runner.kernel_seconds()) +
           "s transfer=" + std::to_string(runner.transfer_seconds()) + "s\n" +
           "gpu phases: preflight=" + std::to_string(t_preflight_s) +
           "s filter_build=" + std::to_string(filter_seconds) +
           "s upload=" + std::to_string(t_upload_s) +
           "s step1_enum(overlapped)=" + std::to_string(st.seconds) +
           "s | consumer dispatch_run=" + std::to_string(t_run_s) +
           "s reduce=" + std::to_string(t_reduce_s) + "s rewalk=" + std::to_string(t_rewalk_s) +
           "s\n" + "gpu run wall: write=" + std::to_string(runner.write_wall_seconds()) +
           "s kernel=" + std::to_string(runner.kernel_wall_seconds()) +
           "s read=" + std::to_string(runner.read_wall_seconds()) + "s\n" +
           "gpu items: survivors=" + std::to_string(total_items) +
           " rewalked=" + std::to_string(rewalk_items) + mode_lines() + startup_lines();
}

// The dispatch counters and walls, the tail build and the transfer counters.
std::string GpuRun::mode_lines() const {
    std::string mode_suffix =
        "\ngpu cpi: wg=" + std::to_string(kCpiWg) + " cap=" + std::to_string(kCpiFlagCap) +
        " batches=" + std::to_string(cpi_batches) +
        " fallback_batches=" + std::to_string(cpi_fallback_batches) +
        " retry_batches=" + std::to_string(cpi_retry_batches) +
        " split_batches=" + std::to_string(cpi_split_batches) +
        " tail_flagged=" + std::to_string(cpi_tail_flagged) +
        " prefix_survivors=" + std::to_string(cpi_prefix_survivors) +
        " device_count=" + std::to_string(runner.device_count_batches()) +
        "\ngpu inflight: inflight_batches=" + std::to_string(runner.inflight_batches()) +
        " inflight_discarded=" + std::to_string(runner.inflight_discarded()) +
        " inflight_wait=" + std::to_string(runner.inflight_wait_seconds()) + "s" +
        "\ngpu cpi wall: pass2=" + std::to_string(runner.cpi_pass2_wall_seconds()) + "s";
    // The selfcheck test reads these keys.
    mode_suffix += "\ngpu transfer: prefetch_hits=" + std::to_string(runner.prefetch_hits()) +
                   " input_check_fail=" + std::to_string(runner.input_check_failures()) +
                   " trimmed_uploads=" + std::to_string(runner.trimmed_batches());
    // The tail_persistent build; the selfcheck test reads this key.
    mode_suffix += "\ngpu tail: tail_persistent " + runner.tail_desc();
    // Batches the mask work-group count rejected, printed only when nonzero so a
    // clean run prints nothing extra; the selfcheck test reads this key.
    if (runner.mask_wg_failures() != 0) {
        mode_suffix += "\ngpu mask: mask_wg_fail=" + std::to_string(runner.mask_wg_failures());
    }
    mode_suffix += "\ngpu tail: persistent-lane (chunk=" + std::to_string(runner.persist_chunk()) +
                   " grid=" + std::to_string(runner.persist_mult()) + "xCU)";
    return mode_suffix;
}

// The startup ordering.
std::string GpuRun::startup_lines() const {
    return std::string("\ngpu startup: filter_s=") + std::to_string(filter_seconds) +
           " bv_upload=" + (upload_chunk_count != 0 ? "staged" : "levels") +
           " upload_s=" + std::to_string(t_upload_s) +
           " upload_end_s=" + std::to_string(t_upload_end_s) +
           " enum_start_s=" + std::to_string(enum_start_s) +
           " first_pickup_s=" + std::to_string(first_pickup_s) +
           " mmap_prefetch=" + (collatz::step2::cache::mmap_prefetch_enabled() ? "1" : "0");
}

} // namespace collatz::gpu::verify_gpu_detail
