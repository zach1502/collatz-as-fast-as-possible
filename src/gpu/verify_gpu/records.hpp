#ifndef COLLATZ_GPU_VERIFY_GPU_RECORDS_HPP
#define COLLATZ_GPU_VERIFY_GPU_RECORDS_HPP

// The exact maximum record and the fold accumulators: the record list settings,
// MaxRecord, the CPU fallback's accumulator and scratch, the preflight folds
// and the exact CPU replay of one candidate.

#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace collatz::gpu::verify_gpu_detail {

// The record list settings of a run (Options::record_threshold), shared
// read-only by every MaxRecord; mirrors RecordThreshold in
// src/core/verify_internal.hpp.
struct RecordList {
    bool on = false;   // a threshold is set
    bool fits = false; // x holds the threshold; false: it exceeds 2^128-1
    collatz::u128 x = 0;
    std::string dec; // canonical decimal of the threshold
    std::size_t cap = collatz::verify::kRecordsCap;
};

// The cap on every flag bound and record gate: X + 1 with a record list whose
// threshold X fits u128 (every excursion above X is then re-walked), else none.
[[nodiscard]] inline collatz::u128 record_bar_cap(const RecordList& rl) noexcept {
    return rl.on && rl.fits && rl.x < collatz::u128_max ? rl.x + 1 : collatz::u128_max;
}

// Running exact global-max record with the smallest-n tie-break. In a frontier
// block, an excursion above 2^128-1 is offered with its GMP decimal: `maxv` then
// holds u128_max and (wide_v, wide_n) the exact record among those. `overflow`
// counts Result::overflow_fallbacks. With a record list, every offered exact
// excursion above the threshold is kept, bounded as in Acc
// (src/core/verify_internal.hpp): once a prune leaves `cap` hits with
// n <= hit_limit, a later hit with a larger n cannot reach Result::records.
struct MaxRecord {
    collatz::u128 maxv = 0;
    collatz::u128 maxn = 0;
    bool have = false;
    std::string wide_v;
    collatz::u128 wide_n = 0;
    bool exceeds = false;       // an offered excursion passed 2^128-1 (max_exceeds_u128)
    std::uint64_t overflow = 0; // Result::overflow_fallbacks
    std::vector<std::pair<collatz::u128, collatz::u128>> hits; // (n, peak), peak < 2^128
    std::vector<collatz::verify::RecordHit> wide_hits;
    collatz::u128 hit_limit = collatz::u128_max;
    bool hits_dropped = false;

    void offer(collatz::u128 exc, collatz::u128 n) noexcept {
        if (!have || exc > maxv || (exc == maxv && n < maxn)) {
            maxv = exc;
            maxn = n;
            have = true;
        }
    }

    // Offer the exact excursion of walked candidate n: `exc`, or, when `dec` is
    // not empty, the decimal `dec` above 2^128-1 (exc is then u128_max).
    void
    offer_exact(const RecordList& rl, collatz::u128 n, collatz::u128 exc, const std::string& dec) {
        offer(exc, n);
        if (!dec.empty()) {
            exceeds = true;
            if (wide_v.empty() || collatz::decimal_less(wide_v, dec) ||
                (dec == wide_v && n < wide_n)) {
                wide_v = dec;
                wide_n = n;
            }
            if (rl.on && collatz::decimal_less(rl.dec, dec)) {
                add_wide_hit(n, dec);
            }
        } else if (rl.on && rl.fits && exc > rl.x) {
            add_hit(rl, n, exc);
        }
    }

    void add_hit(const RecordList& rl, collatz::u128 n, collatz::u128 peak) {
        if (n > hit_limit) {
            hits_dropped = true;
            return;
        }
        hits.emplace_back(n, peak);
        if (hits.size() > rl.cap + rl.cap / 2) {
            prune(rl);
        }
    }

    void add_wide_hit(collatz::u128 n, std::string dec) {
        if (n > hit_limit) {
            hits_dropped = true;
            return;
        }
        wide_hits.push_back(collatz::verify::RecordHit{n, std::move(dec)});
    }

    void prune(const RecordList& rl) {
        collatz::verify::prune_record_hits(hits, wide_hits, rl.cap, hit_limit, hits_dropped);
    }

    void merge(const RecordList& rl, MaxRecord&& o) {
        if (o.have) {
            offer(o.maxv, o.maxn);
        }
        if (!o.wide_v.empty() && (wide_v.empty() || collatz::decimal_less(wide_v, o.wide_v) ||
                                  (o.wide_v == wide_v && o.wide_n < wide_n))) {
            wide_v = o.wide_v;
            wide_n = o.wide_n;
        }
        exceeds = exceeds || o.exceeds;
        overflow += o.overflow;
        hits_dropped = hits_dropped || o.hits_dropped;
        for (const auto& h : o.hits) {
            add_hit(rl, h.first, h.second);
        }
        for (collatz::verify::RecordHit& h : o.wide_hits) {
            add_wide_hit(h.n, std::move(h.peak));
        }
        o.hits.clear();
        o.wide_hits.clear();
    }
};

// Fold accumulator of a batch computed on the CPU (GpuRun::process_job_cpu_exact)
// after it failed its self-check. Every field is a commutative, associative
// fold, so it merges into the consumer's totals like a device batch and the run
// matches a GPU-only run bit for bit.
struct CpuAcc {
    collatz::u128 coverage_raw = 0; // raw sum of mix64(cov_key), reduced mod P at merge
    collatz::u128 pow_raw = 0;      // raw sum of pow digest terms, reduced mod P at merge
    std::uint64_t blocks_total = 0; // wrapping u64 sum of 16-step block counts
    std::uint64_t candidates = 0;   // checked survivor candidates (>= preflimit)
    std::uint64_t masked_out = 0;   // Step-2 pruned extensions (2^A - popcount)
    std::uint64_t rewalk_items = 0; // diagnostic: exact-max reconstructions
    MaxRecord rec;                  // exact max excursion + smallest-n argmax
};

// SoA scratch of the batched CPU tail: finish_block16_n over kCpuTailLanes
// lanes, the CPU verifier's lane driver. It holds kCpuTailCap candidates,
// possibly from several survivors, and is drained when full.
inline constexpr std::size_t kCpuTailCap = 256;
inline constexpr std::size_t kCpuTailLanes = 16;
struct CpuScratch {
    std::array<collatz::u128, kCpuTailCap> n{};
    std::array<collatz::u128, kCpuTailCap> m{};
    std::array<collatz::u128, kCpuTailCap> pmax{};
    std::array<collatz::u128, kCpuTailCap> smax{};
    std::array<unsigned, kCpuTailCap> a{};
    std::array<collatz::step3::BlockOutcome, kCpuTailCap> outs{};
    std::size_t count = 0;
};

// Folds of the exact CPU preflight over every n < preflimit; GpuRun continues
// them.
struct PreflightFolds {
    std::uint64_t coverage = 0;
    std::uint64_t pow_digest = 0;
    std::uint64_t blocks_total = 0;
    std::uint64_t candidates = 0;
    MaxRecord rec;
};

// Exact CPU replay of candidate a of a (lifted) survivor: its excursion
// (first_drop_peak), and the block tail and GMP fallback of verify::run, so the
// excursion, block count and landing value equal the CPU verifier's. The
// device kernels fold a candidate themselves unless its m_a or its 16-step
// recurrence (the shared u128q_tail16 code) overflows u128 or passes
// U128Q_TAIL16_MAX_BLOCKS; such a candidate is flagged and folded once on the
// host from this replay instead (`device_folded` false).
struct Replay {
    collatz::u128 n = 0;
    collatz::u128 exc = 0; // exact excursion; u128_max when it passed 2^128-1
    std::string dec;       // that excursion in decimal, else empty
    std::uint64_t blocks = 0;
    collatz::u128 v_end = 0;
    bool overflowed = false; // the block walk passed 2^128-1 (Result::overflow_fallbacks)
    bool device_folded = true;
};

} // namespace collatz::gpu::verify_gpu_detail

#endif // COLLATZ_GPU_VERIFY_GPU_RECORDS_HPP
