#include "collatz/gpu/bv_split.hpp"
#include "collatz/pow3.hpp"
#include "collatz/verify.hpp"
#include "host_pipeline.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

// Plans the run's device buffers: the split of the Step-2 bitvector table and
// a candidate-id cap that fits the device memory. Returns why the run has to
// use the CPU instead, or an empty string with `layout` filled in.
[[nodiscard]] std::string plan_device_memory(SurvivorRunner& runner,
                                             const collatz::verify::Options& opts,
                                             collatz::verify::Result& res,
                                             BvLayout& layout) {
    const unsigned nbv = opts.num_bitvectors;
    const std::size_t bv_words32_per = (static_cast<std::size_t>(1) << opts.B) / 32u;
    const std::uint64_t bv_bytes =
        static_cast<std::uint64_t>(nbv) * bv_words32_per * sizeof(std::uint32_t);
    // The flat Step-2 bitvector table is split across up to 4 device buffers so
    // no allocation exceeds CL_DEVICE_MAX_MEM_ALLOC_SIZE (at N=44 the table is
    // 2 GB, above the RTX 3070's cap of about 2047 MiB). Each buffer holds a
    // power-of-two run of 8, 4, 2 or 1 levels, the largest that fits.
    // COLLATZ_GPU_MAX_ALLOC can only lower the limit; tests use it to force
    // splits on a device with a larger limit.
    const std::size_t per_level_bytes = bv_words32_per * sizeof(std::uint32_t);
    std::uint64_t max_alloc = runner.device().max_mem_alloc_size;
    if (const char* e = std::getenv("COLLATZ_GPU_MAX_ALLOC")) {
        const std::uint64_t forced = std::strtoull(e, nullptr, 10);
        if (forced > 0 && forced < max_alloc) {
            max_alloc = forced;
        }
    }
    const collatz::gpu::BvSplitPlan split = collatz::gpu::plan_bv_split(opts.B, nbv, max_alloc);
    if (!split.ok) {
        return "gpu verify: Step-2 bitvectors need a " + std::to_string(per_level_bytes) +
               " B per-level buffer <= CL_DEVICE_MAX_MEM_ALLOC_SIZE (" + std::to_string(max_alloc) +
               " B) within 4 buffers; using CPU";
    }
    const int num_bv_buffers = split.num_buffers;
    const cl_uint buf_words_log2 = static_cast<cl_uint>(split.buf_words_log2);
    // Bitvectors plus batch buffers must fit CL_DEVICE_GLOBAL_MEM_SIZE. A fixed
    // batch reserve would not cover the candidate-id buffer (up to 2 GiB, while
    // B=32 puts 4 GiB of bitvectors on an 8 GiB card), so plan_cand_cap counts
    // the real id buffer (cap x bytes per id), the per-survivor buffers of the
    // largest batch, a fixed allowance and driver headroom. If that does not
    // fit, the id cap is lowered, larger batches are split, and a note is
    // printed. COLLATZ_GPU_GLOBAL_MEM can only lower the device size (a test
    // hook, like COLLATZ_GPU_MAX_ALLOC).
    std::uint64_t global_mem = runner.device().global_mem_size;
    if (const char* e = std::getenv("COLLATZ_GPU_GLOBAL_MEM")) {
        const std::uint64_t forced = std::strtoull(e, nullptr, 10);
        if (forced > 0 && forced < global_mem) {
            global_mem = forced;
        }
    }
    {
        const std::size_t cur_cap = runner.cand_id_cap();
        // The serial and in-flight routes share one id list.
        const std::size_t id_bytes = runner.cand_id_bytes();
        // Floor: 256 survivors' worth of candidates (>= 2^A, so splitting an
        // overflowing batch always terminates), and never above the current cap.
        const std::size_t min_cap = std::min<std::size_t>(cur_cap, std::size_t{256} << opts.A);
        const std::size_t max_batch = collatz::gpu::batch_survivor_cap(opts.A, gpu_flush_cap());
        const collatz::gpu::CandCapPlan plan = collatz::gpu::plan_cand_cap(
            global_mem, bv_bytes, max_batch, id_bytes, cur_cap, min_cap, 256);
        if (!plan.fits) {
            return "gpu verify: Step-2 bitvectors (" + std::to_string(bv_bytes) +
                   " B) + batch buffers (" + std::to_string(plan.need - bv_bytes) +
                   " B with the minimum " + std::to_string(min_cap) +
                   " ids) exceed the device budget (" + std::to_string(plan.budget) +
                   " B = CL_DEVICE_GLOBAL_MEM_SIZE " + std::to_string(global_mem) +
                   " B - headroom); using CPU";
        }
        if (plan.lowered) {
            runner.set_cand_budget_cap(plan.cap);
            std::fprintf(stderr,
                         "gpu verify: device memory: candidate-id cap lowered %zu -> %zu ids "
                         "(%zu B/id) so bitvectors %llu B + batch buffers fit %llu B "
                         "(global %llu B - headroom); larger batches are split\n",
                         cur_cap,
                         plan.cap,
                         id_bytes,
                         static_cast<unsigned long long>(bv_bytes),
                         static_cast<unsigned long long>(plan.budget),
                         static_cast<unsigned long long>(global_mem));
        }
        res.gpu_pipeline.budget_cap_ids = plan.lowered ? plan.cap : 0u;
        if (num_bv_buffers > 1 || plan.lowered) {
            std::fprintf(stderr,
                         "gpu verify: device memory estimate %llu B of budget %llu B "
                         "(bitvectors %llu B, id cap %zu x %zu B, global %llu B)\n",
                         static_cast<unsigned long long>(plan.need),
                         static_cast<unsigned long long>(plan.budget),
                         static_cast<unsigned long long>(bv_bytes),
                         plan.cap,
                         id_bytes,
                         static_cast<unsigned long long>(global_mem));
        }
    }
    if (num_bv_buffers > 1) {
        std::fprintf(stderr,
                     "gpu verify: Step-2 bitvectors split across %d buffers (%u levels each, "
                     "max_alloc %llu B)\n",
                     num_bv_buffers,
                     split.levels_per_buffer,
                     static_cast<unsigned long long>(max_alloc));
    }
    layout.num_buffers = num_bv_buffers;
    layout.buf_words_log2 = buf_words_log2;
    return {};
}

GpuRunState::GpuRunState(const collatz::verify::Options& opts_in,
                         SurvivorRunner& runner_in,
                         const collatz::step2::Filter& filter_in,
                         collatz::verify::Result& res_in,
                         const RunStart& start,
                         PreflightFolds&& pre)
    : opts(opts_in), runner(runner_in), filter(filter_in), res(res_in), t_start(start.t_start),
      build_threads(start.build_threads), t_preflight_s(start.t_preflight_s),
      filter_seconds(start.filter_seconds), preflimit(start.preflimit),
      num_bv_buffers(start.layout.num_buffers), buf_words_log2(start.layout.buf_words_log2),
      coverage(pre.coverage), pow_digest(pre.pow_digest), blocks_total(pre.blocks_total),
      candidates(pre.candidates), rec(std::move(pre.rec)) {}

// Device setup: the Step-2 bitvectors and the upload-trim tables. Runs at the
// top of the consumer thread.
void GpuRun::upload_setup() {
    const auto t_up0 = Clock::now();
    // Write each level straight from the filter's packed u64 words (on a
    // little-endian host their bytes are the flattened uint32 table) and
    // finish with clFinish, so no transfer is left for the first kernel.
    std::vector<const void*> levels(nbv);
    const auto& bvs = filter.bitvectors();
    for (unsigned i = 0; i < nbv; ++i) {
        levels[i] = bvs[i].data();
    }
    // The staged upload streams the same levels through pinned chunks,
    // so reading the mapped table overlaps the DMA; the device bytes are
    // the same. On failure the level-by-level upload below recreates the
    // buffers.
    bool staged = false;
    try {
        upload_chunk_count = runner.upload_bitvector_levels_staged(
            levels,
            bv_words32_per,
            num_bv_buffers,
            buf_words_log2,
            collatz::gpu::env_size_or("COLLATZ_GPU_EARLY_UPLOAD_CHUNK_MB", 64) << 20);
        staged = true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "gpu: staged bitvector upload failed (%s); using the "
                     "level-by-level upload\n",
                     ex.what());
        upload_chunk_count = 0;
    }
    if (!staged) {
        runner.upload_bitvector_levels(levels, bv_words32_per, num_bv_buffers, buf_words_log2);
    }
    // Upload trim: a per-f table of the f-dependent kernel inputs, derived as
    // build_inputs does (f in [fmin, M]; every survivor has f <= M), so it
    // covers every survivor of every batch. The cpi kernels read those inputs
    // only from this table. With M < fmin no survivor reaches a batch.
    if (M >= fmin && M - fmin < 256u) {
        std::vector<cl_uint4> p3tab;
        std::vector<cl_uint4> fcol;
        for (unsigned f = fmin; f <= M; ++f) {
            const unsigned i = f - fmin;
            cl_uint4 col{};
            if (i >= nbv) {
                col.s[2] = 1u; // bvmode: all-ones window
            } else {
                col.s[0] = static_cast<cl_uint>(filter.inv3f(i));
                col.s[1] = static_cast<cl_uint>(static_cast<std::size_t>(i) * bv_words32_per);
            }
            fcol.push_back(col);
            p3tab.push_back(to_uint4(collatz::pow3(f)));
        }
        runner.set_trim_tables(fmin, p3tab, fcol);
    }
    t_upload_s = std::chrono::duration<double>(Clock::now() - t_up0).count();
    t_upload_end_s = std::chrono::duration<double>(Clock::now() - t_start).count();
}

// Producer-side packing: producer threads pack each batch's staged upload
// (the pinned, trimmed layout: n0 and m narrowed to u64, astar, a 1-byte f
// index) into a block of a runner-owned pool before pushing it, so the dispatch
// thread only enqueues the writes. The pool is created here, before the
// consumer and producers start, so its spec is read-only while they run. The
// packed bytes are exactly pack_staged's, and a batch whose block does not
// match is packed on the dispatch thread instead.
void GpuRun::init_producer_prep() {
    // Same condition as the set_trim_tables call in upload_setup.
    const bool trim_tables = M >= fmin && M - fmin < 256u;
    const std::size_t pool_blocks = kMaxInFlight + 5; // queue + lookahead + 2 in flight + slack
    producer_prep = runner.init_pack_pool(trim_tables,
                                          fmin,
                                          trim_tables ? static_cast<std::size_t>(M - fmin + 1u) : 0,
                                          flush_cap,
                                          pool_blocks);
}

} // namespace collatz::gpu::verify_gpu_detail
