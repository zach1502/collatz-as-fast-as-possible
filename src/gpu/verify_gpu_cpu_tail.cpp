#include "collatz/gpu/u128_limbs.h"
#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "verify_gpu/gpu_run.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

// Drains the CPU tail's SoA scratch: runs the lane driver, then folds each
// candidate into `acc`.
void GpuRun::flush_cpu_tail(CpuAcc& acc, CpuScratch& sc) const {
    if (sc.count == 0) {
        return;
    }
    // The running record is the threshold, as in the CPU verifier's
    // flush_tail_batch, so the block kernel skips the exact interior-block
    // maximum for candidates that cannot beat the record; thr = 0 would force
    // the slower exact-max mode for every candidate. The gate below reads
    // tail_max only to pick the contenders (tail_ge), and it is exact whenever
    // it reaches the record, so the threshold does not change the result. With
    // a record list the bar is capped at X + 1 (gate_bar in
    // src/core/verify_internal.hpp).
    const auto bar = [&]() {
        return acc.rec.have ? capped(acc.rec.maxv) : static_cast<collatz::u128>(0);
    };
    const collatz::u128 rrec0 = bar();
    const collatz::u128 thr = rrec0 > 0 ? rrec0 - 1 : 0;
    collatz::step3::finish_block16_n(
        sc.n.data(), sc.m.data(), sc.count, kCpuTailLanes, thr, sc.outs.data());
    collatz::u128 rrec = rrec0;
    std::string dec;
    for (std::size_t i = 0; i < sc.count; ++i) {
        std::uint64_t blocks = 0;
        collatz::u128 tail_max = 0;
        collatz::u128 v_end = 0;
        dec.clear();
        if (sc.outs[i].overflow) {
            const collatz::step3::BlockOutcome ko =
                collatz::step3::finish_block16_oracle(sc.n[i], sc.m[i], &dec);
            blocks = ko.blocks;
            tail_max = ko.max_seen;
            v_end = ko.v_end;
        } else {
            blocks = sc.outs[i].blocks;
            tail_max = sc.outs[i].max_seen;
            v_end = sc.outs[i].v_end;
        }
        acc.blocks_total += blocks;
        // Raw u128 sum, reduced mod P once at merge, like the CPU
        // verifier's acc.pow_raw.
        acc.pow_raw += u128q_result_digest_term(
            u128q_from_halves(collatz::lo64(sc.n[i]), collatz::hi64(sc.n[i])),
            blocks,
            u128q_from_halves(collatz::lo64(v_end), collatz::hi64(v_end)));
        if (!dec.empty()) {
            ++acc.rec.overflow; // the block walk passed 2^128-1 (tail_max is u128_max)
        }
        // Threshold-guarded exact-max reconstruction, as in the CPU verifier's
        // finalize_record_contender: tail_max is exact when it reaches the
        // record, and it bounds the excursion from above. A tail contender's
        // whole excursion is walked; otherwise only the M-step prefix, which
        // decides it because the tail stays below the record.
        const bool tail_ge = tail_max >= rrec;
        const collatz::u128 pub = prefix_ub(sc.pmax[i], sc.smax[i], sc.a[i]);
        if (!tail_ge && pub < rrec) {
            continue; // provably below the record: no exact work
        }
        ++acc.rewalk_items;
        const collatz::step3::Outcome q =
            collatz::step3::first_drop_peak(sc.n[i], &dec, tail_ge ? ~0u : M);
        acc.rec.offer_exact(rl, sc.n[i], q.max_seen, dec);
        rrec = bar();
    }
    acc.candidates += sc.count;
    sc.count = 0;
}

// Finish survivors [begin, end) of a batch on the CPU, folding into a CpuAcc;
// this is the CPU counterpart of the device path. It expands each
// survivor's Step-2 mask (filter.mask, the same bits as the device kernel),
// runs the CPU verifier's lane driver (finish_block16_n over kCpuTailLanes
// lanes) on the surviving extensions, and folds coverage, pow digest, block
// and candidate counts, the masked-out count and the exact max as the device
// reduce and re-walk do. Coverage depends only on n and is folded when a
// candidate is queued; the rest is folded once its tail resolves. All folds
// are commutative, so it does not matter which batches the CPU takes.
void GpuRun::process_chunk_cpu(const std::vector<collatz::step1::Node>& nodes,
                               std::size_t begin,
                               std::size_t end,
                               CpuAcc& acc,
                               CpuScratch& sc) const {
    for (std::size_t ni = begin; ni < end; ++ni) {
        const collatz::step1::Node& node = nodes[ni];
        const collatz::step2::CandidateMask mask = filter.mask(node);
        const unsigned pc = mask.popcount();
        acc.masked_out += twoA - pc;
        if (pc == 0) {
            continue;
        }
        const collatz::u128 p3f = collatz::pow3(node.f);
        for (unsigned wi = 0; wi < mask.words; ++wi) {
            std::uint64_t bits = mask.w[wi];
            const unsigned base = wi * 64u;
            while (bits != 0) {
                const unsigned a = base + static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const collatz::u128 n = node.n0 + (static_cast<collatz::u128>(a) << M);
                if (n < preflimit) {
                    continue; // already discharged by the preflight
                }
                bool of = false;
                const collatz::u128 term =
                    collatz::checked_mul(p3f, static_cast<collatz::u128>(a), of);
                collatz::u128 m_a = 0;
                if (!of) {
                    m_a = collatz::checked_add(node.m, term, of);
                }
                // Coverage depends only on n, so it is folded here, as in the
                // CPU verifier: a raw u128 sum, reduced mod P once at merge.
                acc.coverage_raw += u128q_mix64(cov_key(n));
                if (of) {
                    // m_a beyond u128 (unreachable for N <= 64 and for a block):
                    // fold via the GMP block replay directly, outside the SoA batch.
                    std::string odec;
                    const collatz::step3::BlockOutcome ko =
                        collatz::step3::finish_block16_oracle(n, n, &odec);
                    acc.blocks_total += ko.blocks;
                    acc.pow_raw += u128q_result_digest_term(
                        u128q_from_halves(collatz::lo64(n), collatz::hi64(n)),
                        ko.blocks,
                        u128q_from_halves(collatz::lo64(ko.v_end), collatz::hi64(ko.v_end)));
                    ++acc.candidates;
                    if (!odec.empty()) {
                        ++acc.rec.overflow;
                    }
                    const collatz::step3::Outcome q = collatz::step3::first_drop_peak(n, &odec);
                    acc.rec.offer_exact(rl, n, q.max_seen, odec);
                    continue;
                }
                const std::size_t i = sc.count;
                sc.n[i] = n;
                sc.m[i] = m_a;
                sc.pmax[i] = node.pmax;
                sc.smax[i] = node.smax;
                sc.a[i] = a;
                ++sc.count;
                if (sc.count >= kCpuTailCap) {
                    flush_cpu_tail(acc, sc);
                }
            }
        }
    }
    flush_cpu_tail(acc, sc);
}

// Exact CPU processing of a whole survivor batch, folding into the
// consumer-owned run totals: the self-check's last resort after a batch failed
// its re-runs. It runs process_chunk_cpu into a scratch
// CpuAcc and merges it, so the folds are bit-identical.
void GpuRun::process_job_cpu_exact(Job& job) {
    if (job.nodes.empty()) {
        return;
    }
    CpuAcc acc;
    process_chunk_cpu(job.nodes, 0, job.nodes.size(), acc, fallback_scratch);
    coverage = static_cast<std::uint64_t>(
        u128q_mod_mersenne61(coverage + mod_mersenne61_u128(acc.coverage_raw)));
    pow_digest = static_cast<std::uint64_t>(
        u128q_mod_mersenne61(pow_digest + mod_mersenne61_u128(acc.pow_raw)));
    blocks_total += acc.blocks_total;
    candidates += acc.candidates;
    masked_out += acc.masked_out;
    rewalk_items += acc.rewalk_items;
    rec.merge(rl, std::move(acc.rec));
}

} // namespace collatz::gpu::verify_gpu_detail
