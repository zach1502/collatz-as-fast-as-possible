#include "collatz/gpu/u128_limbs.h"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

inline collatz::u128 GpuRun::cand_n(const collatz::step1::Node& node, unsigned a) const {
    return node.n0 + (static_cast<collatz::u128>(a) << M);
}

// `n` is cand_n(node, a); callers skip the preflight range before replaying.
// The excursion is first_drop_peak(n), as in the CPU verifier's record
// contender; the block walk gives the block count, the landing value and
// `overflowed` (that walk passed 2^128-1, the CPU's overflow_fallbacks rule).
Replay GpuRun::replay(const collatz::step1::Node& node, unsigned a, collatz::u128 n) const {
    Replay r;
    r.n = n;
    const collatz::step3::Outcome q = collatz::step3::first_drop_peak(n, &r.dec);
    r.exc = q.max_seen;
    const collatz::u128 p3f = collatz::pow3(node.f);
    bool of = false;
    const collatz::u128 term = collatz::checked_mul(p3f, static_cast<collatz::u128>(a), of);
    collatz::u128 m_a = 0;
    if (!of) {
        m_a = collatz::checked_add(node.m, term, of);
    }
    std::string wdec;
    if (of) {
        // m_a beyond u128 (unreachable for N <= 64 and for a block, where
        // validate_block keeps it below 2^125): as on the CPU, the whole walk
        // from n is replayed in GMP.
        const collatz::step3::BlockOutcome ko =
            collatz::step3::finish_block16_oracle(r.n, r.n, &wdec);
        r.blocks = ko.blocks;
        r.v_end = ko.v_end;
        r.overflowed = !wdec.empty();
        r.device_folded = false;
        return r;
    }
    const collatz::step3::BlockOutcome bo = collatz::step3::finish_block16(r.n, m_a, 0);
    if (bo.overflow) {
        const collatz::step3::BlockOutcome ko =
            collatz::step3::finish_block16_oracle(r.n, m_a, &wdec);
        r.blocks = ko.blocks;
        r.v_end = ko.v_end;
        // finish_block16 also overflows when an exactly walked block peak
        // passes u128, the device only when a landing value does, so the
        // device code (u128q_tail16) decides device_folded.
        u128q dmx;
        u128q dve;
        u128q_u32 dovf = 0;
        (void) u128q_tail16(u128q_from_halves(collatz::lo64(r.n), collatz::hi64(r.n)),
                            u128q_from_halves(collatz::lo64(m_a), collatz::hi64(m_a)),
                            tbl16_host.data(),
                            &dmx,
                            &dovf,
                            &dve);
        r.device_folded = dovf == 0u;
    } else {
        r.blocks = bo.blocks;
        r.v_end = bo.v_end;
        r.device_folded = bo.blocks <= U128Q_TAIL16_MAX_BLOCKS;
    }
    r.overflowed = !wdec.empty();
    return r;
}

// Fold a candidate the device skipped (Replay::device_folded false), exactly
// as the device folds the others.
inline void GpuRun::fold_replay(const Replay& r) {
    coverage = static_cast<std::uint64_t>(
        u128q_mod_mersenne61(coverage + u128q_mod_mersenne61(u128q_mix64(cov_key(r.n)))));
    pow_digest = static_cast<std::uint64_t>(u128q_mod_mersenne61(
        pow_digest + u128q_mod_mersenne61(digest_term(r.n, r.blocks, r.v_end))));
    blocks_total += r.blocks;
    ++candidates;
}

// A candidate the device processed and flagged (or one of a re-walked
// survivor of the per-survivor kernel): offer its exact excursion, count it
// toward overflow_fallbacks and fold it when the device skipped it. Each such
// candidate reaches here once per run, since a device dispatch flags each
// candidate at most once and a re-walked survivor is visited once.
inline void GpuRun::settle_candidate(const collatz::step1::Node& node, unsigned a) {
    const collatz::u128 n = cand_n(node, a);
    if (n < preflimit) {
        return;
    }
    const Replay r = replay(node, a, n);
    rec.offer_exact(rl, r.n, r.exc, r.dec);
    if (r.overflowed) {
        ++rec.overflow;
    }
    if (!r.device_folded) {
        fold_replay(r);
    }
}

// Re-walk a single candidate n = node.n0 + a*2^M exactly on the CPU
// and offer its excursion to the record. The host prefix-bound re-walks use
// it; they may revisit a device-flagged candidate, so it neither folds nor
// counts.
inline void GpuRun::rewalk_candidate(const collatz::step1::Node& node, unsigned a) {
    const collatz::u128 n = cand_n(node, a);
    if (n < preflimit) {
        return;
    }
    const Replay r = replay(node, a, n);
    rec.offer_exact(rl, r.n, r.exc, r.dec);
}

// Per-survivor kernel path: dispatch one job, reduce its folds and re-walk
// its contenders. It runs on the consumer thread only, so the fold state
// needs no lock.
void GpuRun::process_job(Job& job) {
    SurvivorBatch& batch = job.batch;
    std::vector<collatz::step1::Node>& nodes = job.nodes;
    if (batch.size() == 0) {
        return;
    }
    const auto t_r0 = Clock::now();
    const auto run_once = [&] { runner.run(batch, kc, out); };
    const bool sc_ok = sc_run(run_once, run_once, [&]() -> const char* {
        return out.sc_ok ? nullptr : (out.sc_why != nullptr ? out.sc_why : "unknown");
    });
    t_run_s += std::chrono::duration<double>(Clock::now() - t_r0).count();
    if (!sc_ok) {
        process_job_cpu_exact(job);
        return;
    }
    total_items += batch.size();
    const auto t_rd0 = Clock::now();
    collatz::u128 chunk_bs = 0;
    for (std::size_t j = 0; j < batch.size(); ++j) {
        coverage = static_cast<std::uint64_t>(u128q_mod_mersenne61(coverage + out.cov[j]));
        pow_digest = static_cast<std::uint64_t>(u128q_mod_mersenne61(pow_digest + out.dig[j]));
        blocks_total += out.blocks[j];
        candidates += out.cand[j];
        masked_out += twoA - out.pc[j];
        const collatz::u128 iv = from_uint4(out.maxv[j]);
        if (iv > chunk_bs) {
            chunk_bs = iv;
        }
    }
    // The batch's block-start maximum serves as a first bound. It need not be
    // below every excursion (a block start can follow a mid-block drop below
    // its n), so when the record stays below it after the first pass, the
    // survivors between the record and it are re-walked too.
    const collatz::u128 R0 = flag_bound();
    const collatz::u128 Rlb = capped(chunk_bs) > R0 ? capped(chunk_bs) : R0;
    add_elapsed(t_reduce_s, t_rd0);
    // Flag and exactly re-walk the record-contending survivors on the CPU. A
    // survivor holding an overflowed candidate always has ovf set, so every
    // candidate the kernel skipped is settled in the first pass, and the
    // second pass only offers excursions (rewalk_candidate).
    const auto t_rw0 = Clock::now();
    const auto walk_survivor = [&](const collatz::step1::Node& node, bool settle) {
        ++rewalk_items;
        const collatz::step2::CandidateMask mask = filter.mask(node);
        for (unsigned wi = 0; wi < mask.words; ++wi) {
            std::uint64_t bits = mask.w[wi];
            const unsigned base = wi * 64u;
            while (bits != 0) {
                const unsigned a = base + static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                if (settle) {
                    settle_candidate(node, a);
                } else {
                    rewalk_candidate(node, a);
                }
            }
        }
    };
    const auto reaches = [&](std::size_t j, collatz::u128 bound) {
        return out.ovf[j] != 0 || tail_ub(from_uint4(out.maxv[j])) >= bound ||
               prefix_ub(nodes[j].pmax, nodes[j].smax, twoA - 1u) >= bound;
    };
    for (std::size_t j = 0; j < batch.size(); ++j) {
        if (reaches(j, Rlb)) {
            walk_survivor(nodes[j], true);
        }
    }
    const collatz::u128 Rok = flag_bound();
    for (std::size_t j = 0; Rok < Rlb && j < batch.size(); ++j) {
        if (!reaches(j, Rlb) && reaches(j, Rok)) {
            walk_survivor(nodes[j], false);
        }
    }
    add_elapsed(t_rewalk_s, t_rw0);
}

// Folds a validated CPI batch into the run totals and re-walks its record
// contenders exactly: the candidates the device tail flagged, and every
// candidate of a survivor whose host prefix bound reaches the record.
void GpuRun::fold_cpi_batch(Job& job, const CpiResult& cr) {
    SurvivorBatch& batch = job.batch;
    std::vector<collatz::step1::Node>& nodes = job.nodes;
    ++cpi_batches;
    if (!cr.sc_checked) {
        ++sc_unchecked;
    }
    total_items += batch.size();
    const auto t_rd0 = Clock::now();
    coverage = static_cast<std::uint64_t>(u128q_mod_mersenne61(coverage + cr.coverage));
    pow_digest = static_cast<std::uint64_t>(u128q_mod_mersenne61(pow_digest + cr.digest));
    blocks_total += cr.blocks;
    candidates += cr.processed;
    masked_out += static_cast<std::uint64_t>(batch.size()) * twoA - cr.sum_pc;
    collatz::u128 Rlb_now = flag_bound();
    add_elapsed(t_reduce_s, t_rd0);
    const auto t_rw0 = Clock::now();
    const std::uint32_t twoA_mask = twoA - 1u;
    for (const std::uint32_t packed : cr.flags) {
        const std::uint32_t s = packed >> opts.A;
        const unsigned a = static_cast<unsigned>(packed & twoA_mask);
        settle_candidate(nodes[s], a);
        ++cpi_tail_flagged;
        ++rewalk_items;
    }
    // Re-walks every candidate of each survivor whose prefix bound reaches
    // Rlb_now. No survivor does when the batch bound pub_max is below Rlb_now,
    // so the loop then runs over kNoNodes.
    for (const collatz::step1::Node& node : job.pub_max < Rlb_now ? kNoNodes : nodes) {
        if (prefix_ub(node.pmax, node.smax, twoA - 1u) < Rlb_now) {
            continue;
        }
        ++cpi_prefix_survivors;
        const collatz::step2::CandidateMask mask = filter.mask(node);
        for (unsigned wi = 0; wi < mask.words; ++wi) {
            std::uint64_t bits = mask.w[wi];
            const unsigned base = wi * 64u;
            while (bits != 0) {
                const unsigned a = base + static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                rewalk_candidate(node, a);
                ++rewalk_items;
            }
        }
    }
    add_elapsed(t_rewalk_s, t_rw0);
}

// After a flag-overflow retry dispatched with rlb = the batch's block-start
// maximum (capped): offers the exact excursion of every flagged candidate, and
// returns whether the record then reaches rlb. Only then does every candidate
// whose excursion can equal the final record carry a flag at rlb (a block start
// can follow a mid-block drop below its n, so rlb itself is no lower bound);
// otherwise the caller dispatches the batch again with flag_bound(). The
// offers are exact and fold nothing, so repeating them later is harmless.
bool GpuRun::retry_bound_holds(const Job& job, const CpiResult& cr, collatz::u128 rlb) {
    const std::uint32_t twoA_mask = twoA - 1u;
    std::string dec;
    for (const std::uint32_t packed : cr.flags) {
        const collatz::u128 n =
            cand_n(job.nodes[packed >> opts.A], static_cast<unsigned>(packed & twoA_mask));
        if (n < preflimit) {
            continue;
        }
        const collatz::step3::Outcome q = collatz::step3::first_drop_peak(n, &dec);
        rec.offer_exact(rl, n, q.max_seen, dec);
    }
    return flag_bound() >= rlb;
}

} // namespace collatz::gpu::verify_gpu_detail
