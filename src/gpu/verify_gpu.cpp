#include "collatz/gpu/u128_limbs.h"
#include "collatz/step2.hpp"
#include "collatz/step3.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "survivor_runtime.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace collatz::gpu {

using namespace verify_gpu_detail;

namespace {

// Verifies every n < preflimit exactly on the CPU, with the block digest,
// coverage and record of the CPU verifier's preflight (process_candidate):
// every n whose block walk reaches the record bar offers its excursion
// (first_drop_peak) to the record and to the record list.
[[nodiscard]] PreflightFolds run_preflight(collatz::u128 preflimit, const RecordList& rl) {
    PreflightFolds p;
    const collatz::u128 cap = record_bar_cap(rl);
    std::string dec;
    for (collatz::u128 n = 1; n < preflimit; ++n) {
        const collatz::step3::BlockOutcome bo = collatz::step3::finish_block16(n, n, 0);
        p.coverage = static_cast<std::uint64_t>(
            u128q_mod_mersenne61(p.coverage + u128q_mod_mersenne61(u128q_mix64(cov_key(n)))));
        p.pow_digest = static_cast<std::uint64_t>(u128q_mod_mersenne61(
            p.pow_digest + u128q_mod_mersenne61(digest_term(n, bo.blocks, bo.v_end))));
        p.blocks_total += bo.blocks;
        ++p.candidates;
        const collatz::u128 bar =
            p.rec.have ? (p.rec.maxv < cap ? p.rec.maxv : cap) : static_cast<collatz::u128>(0);
        if (bo.max_seen >= bar) {
            const collatz::step3::Outcome q = collatz::step3::first_drop_peak(n, &dec);
            p.rec.offer_exact(rl, n, q.max_seen, dec);
        }
    }
    return p;
}

} // namespace

collatz::verify::Result
verify_gpu(const collatz::verify::Options& opts, bool* used_gpu, std::string* msg) {
    const auto set_used = [used_gpu](bool v) {
        if (used_gpu != nullptr) {
            *used_gpu = v;
        }
    };
    const auto set_msg = [msg](const std::string& m) {
        if (msg != nullptr) {
            *msg = m;
        }
    };

    // The device mask runs one work-group of 2^A/32 lanes per survivor, so the
    // GPU path needs A >= 5; anything else, or a missing or unsuitable device,
    // falls back to the exact CPU verifier. The bitvector upload, plan_bv_split
    // and the window reads work in whole 64-bit words per level (2^B / 32 u32
    // words), so B < 6 takes the CPU path too, as does an out-of-range B, which
    // verify::run then rejects. N is the Step-1 depth (K for a block); the value
    // width is value_bits(opts), up to 96 bits in block mode, and the device
    // carries n, m and every tail value as 128-bit limbs.
    if (opts.A < 5 || opts.A > collatz::step2::max_A || opts.N <= opts.A || opts.N > 64 ||
        opts.B < 6 || opts.B > collatz::step2::max_B) {
        set_used(false);
        set_msg("gpu verify: unsupported options (needs 5 <= A <= 14, A < N <= 64, "
                "6 <= B <= max_B); using CPU");
        return collatz::verify::run(opts);
    }
    // Frontier block mode has the preconditions of the CPU path (validate_block
    // throws std::invalid_argument). The producers lift every survivor into the
    // block before Step 2 (append_node), so the device kernels see the block
    // members themselves.
    collatz::verify::validate_block(opts);
    const bool block = opts.block_h != 0;
    const unsigned vbits = collatz::verify::value_bits(opts);
    std::string err;
    // create() gets the run's value width, which turns COLLATZ_GPU_TAIL_U64N off
    // when some n can reach 2^64.
    TailHint tail_hint;
    tail_hint.value_bits = vbits;
    const unsigned build_threads = opts.threads == 0 ? collatz::hardware_threads() : opts.threads;
    collatz::step2::Params params_c{
        opts.N, opts.A, opts.B, opts.num_bitvectors, build_threads, opts.partial_oee};
    params_c.use_bv_cache = opts.bv_cache;                 // the CPU path's table cache
    params_c.window_chains = opts.window_chains;           // window chain-merge dip terms
    params_c.window_templates_D = opts.window_templates_D; // general merge templates
    // Built after the device is up (below). Declared ahead of the runner so the
    // runner, which reads its bitvectors, is torn down first.
    std::optional<collatz::step2::Filter> filter_storage;
    // Device and program setup, including clBuildProgram of every kernel source,
    // runs before t_start, so it is outside seconds_total.
    std::unique_ptr<SurvivorRunner> runner_ptr = SurvivorRunner::create(&err, tail_hint);
    if (!runner_ptr) {
        set_used(false);
        set_msg("gpu verify: no usable OpenCL GPU (" + err + "); using CPU");
        return collatz::verify::run(opts);
    }
    SurvivorRunner& runner = *runner_ptr;
    // The device mask runs one work-group of 2^A/32 work-items per survivor. A
    // device that allows fewer for that kernel cannot launch it, so the run
    // uses the CPU here rather than failing at its first batch.
    const std::size_t mask_local = (std::size_t{1} << opts.A) / 32u;
    if (mask_local > runner.mask_local_max()) {
        set_used(false);
        set_msg("gpu verify: the device mask needs work-groups of " + std::to_string(mask_local) +
                " work-items at A=" + std::to_string(opts.A) + ", above this device's limit of " +
                std::to_string(runner.mask_local_max()) + "; using CPU");
        return collatz::verify::run(opts);
    }
    set_used(true);
    // Test hook: COLLATZ_GPU_SELFCHECK_INJECT=<dispatch>[,<count>] corrupts the
    // host-side readback of `count` (default 1) self-checked dispatches, starting
    // at 0-based dispatch index <dispatch>.
    if (const char* e = std::getenv("COLLATZ_GPU_SELFCHECK_INJECT"); e != nullptr && *e != '\0') {
        char* endp = nullptr;
        const unsigned long long first = std::strtoull(e, &endp, 10);
        unsigned long long cnt = 1;
        if (endp != nullptr && *endp == ',') {
            cnt = std::strtoull(endp + 1, nullptr, 10);
        }
        runner.set_selfcheck_inject(first, cnt);
    }

    collatz::verify::Result res;
    const auto t_start = Clock::now();

    // A block has no preflight (validate_block keeps L >= 2^17); preflimit = 1
    // then skips nothing and every preflight_* field stays 0, as on the CPU.
    const unsigned preflight_bits =
        block ? 0u : (opts.N < kPreflightBits ? opts.N : kPreflightBits);
    const collatz::u128 preflimit = static_cast<collatz::u128>(1) << preflight_bits;

    // Preflight: every n < min(2^17, 2^N), exactly on the CPU (run_preflight).
    const auto t_pre0 = Clock::now();
    PreflightFolds pre = run_preflight(preflimit, record_list(opts));
    const double t_preflight_s = std::chrono::duration<double>(Clock::now() - t_pre0).count();
    // Every case c/k slice re-runs this preflight unsliced, so `collatz merge`
    // subtracts k-1 copies of it using the result's `preflight` block, which the
    // GPU path fills like verify_step1.cpp. Left at 0, a merge of GPU slices would
    // count the preflight k times. Only the preflight has folded so far, so the
    // totals are its partials, in the CPU snapshot's reductions (coverage and pow
    // mod 2^61-1, blocks and steps wrapping u64; here step3_steps_total ==
    // blocks_total).
    res.preflight_candidates = pre.candidates;
    res.preflight_coverage = pre.coverage;
    res.preflight_pow = pre.pow_digest;
    res.preflight_blocks = pre.blocks_total;
    res.preflight_step3_steps = pre.blocks_total;

    // Build the Step-2 filter once; GpuRun::upload_setup copies its bitvectors to
    // the device.
    const auto t_f0 = Clock::now();
    const collatz::step2::Filter& filter = filter_storage.emplace(params_c);
    const double filter_seconds = std::chrono::duration<double>(Clock::now() - t_f0).count();
    res.seconds_filter = filter_seconds;
    res.bv_cache_hit = filter.bv_cache_hit();

    BvLayout layout;
    if (const std::string why = plan_device_memory(runner, opts, res, layout); !why.empty()) {
        set_used(false);
        set_msg(why);
        // The CPU verifier builds its own filter, so this one and the device
        // go first; at B=32 a second filter would double the table memory.
        filter_storage.reset();
        runner_ptr.reset();
        return collatz::verify::run(opts);
    }
    GpuRun run(opts,
               runner,
               filter,
               res,
               RunStart{.t_start = t_start,
                        .build_threads = build_threads,
                        .t_preflight_s = t_preflight_s,
                        .filter_seconds = filter_seconds,
                        .preflimit = preflimit,
                        .layout = layout},
               std::move(pre));
    // A failure on the dispatch thread or a producer (an OpenCL error after
    // setup) is rethrown here after every thread has joined, and the run ends
    // without a result: `collatz verify` reports it and exits with 2, and a
    // --checkpoint run keeps its finished slices. Finishing the remaining
    // batches on the CPU would run them all on the one dispatch thread.
    try {
        run.run_pipeline();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("gpu verify: the GPU run failed (") + e.what() +
                                 "); no result");
    } catch (...) {
        throw std::runtime_error("gpu verify: the GPU run failed (unknown error); no result");
    }
    run.finish();
    set_msg(run.summary_message());
    return res;
}

} // namespace collatz::gpu
