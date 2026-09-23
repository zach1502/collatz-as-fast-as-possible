// The exact merge of slice Results: merge and its modular and wrapping sums.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

namespace collatz::checkpoint {

using namespace detail;

std::uint64_t merge_mod_p(const std::vector<std::uint64_t>& parts, std::uint64_t pre) {
    // Every term is reduced mod P first, so the u128 sum of up to 2^64 terms
    // cannot overflow (each term < 2^61). Subtracting (k-1)*pre mod P is done
    // as adding (k-1)*(P - pre mod P) mod P, keeping everything non-negative.
    u128 acc = 0;
    for (const std::uint64_t x : parts) {
        acc += x % kP61;
    }
    acc %= kP61;
    const std::uint64_t km1 = parts.empty() ? 0 : static_cast<std::uint64_t>(parts.size() - 1);
    const u128 neg_pre = (static_cast<u128>(kP61) - (pre % kP61)) % kP61;
    acc = (acc + (static_cast<u128>(km1 % kP61) * neg_pre) % kP61) % kP61;
    return static_cast<std::uint64_t>(acc);
}

std::uint64_t merge_wrap64(const std::vector<std::uint64_t>& parts, std::uint64_t pre) {
    std::uint64_t acc = 0;
    for (const std::uint64_t x : parts) {
        acc += x; // wrapping u64, as the slices themselves accumulate
    }
    const std::uint64_t km1 = parts.empty() ? 0 : static_cast<std::uint64_t>(parts.size() - 1);
    return acc - km1 * pre; // wrapping
}

namespace {

// Every distinct backend the slices ran on, as one label ("mixed: a + b").
std::string backend_label(const std::vector<Slice>& slices) {
    std::vector<std::string> bes;
    for (const Slice& s : slices) {
        if (std::find(bes.begin(), bes.end(), s.backend) == bes.end()) {
            bes.push_back(s.backend);
        }
    }
    std::string label = bes.size() == 1 ? bes[0] : "mixed:";
    if (bes.size() > 1) {
        for (std::size_t i = 0; i < bes.size(); ++i) {
            label += (i == 0 ? " " : " + ") + bes[i];
        }
    }
    return label;
}

} // namespace

bool merge(const std::vector<Slice>& slices,
           verify::Result& out,
           std::string& err,
           MergeInfo* info) {
    if (slices.empty()) {
        err = "no slices to merge";
        return false;
    }
    const std::uint64_t K = slices[0].k;
    if (!check_slice_set(slices, K, err)) {
        return false;
    }
    if (!check_slices_match(slices, err)) {
        return false;
    }
    const Slice& s0 = slices[0];
    // Options verify cannot run never came from it; a block_h near 2^128 would
    // also wrap the (block_h + 1) of the max_exact bound below.
    try {
        verify::Options vo = s0.opts;
        vo.N = s0.N;
        vo.block_h = s0.block_h;
        verify::validate_block(vo);
    } catch (const std::exception& e) {
        err = s0.source + ": " + e.what();
        return false;
    }
    const verify::Result& pre = s0.r;
    const std::uint64_t km1 = K - 1;
    verify::Result r;
    u128 cand = 0;
    std::vector<std::uint64_t> steps;
    std::vector<std::uint64_t> blocks;
    std::vector<std::uint64_t> cov;
    std::vector<std::uint64_t> pow;
    std::vector<verify::RecordHit> hits;
    std::string max_dec;
    // A slice whose max passed 2^128 without the exact wide value (a GPU slice,
    // or a file without max_excursion_wide) has an unknown maximum: it may beat
    // any other slice's record, so the merged max cannot be certified exact.
    bool saturated_unknown = false;
    for (const Slice& s : slices) {
        cand += s.r.candidates_checked;
        r.step2_masked_out += s.r.step2_masked_out;
        r.step1.survivors += s.r.step1.survivors;
        r.overflow_fallbacks += s.r.overflow_fallbacks;
        r.prefix_walks += s.r.prefix_walks;
        steps.push_back(s.r.step3_steps_total);
        blocks.push_back(s.r.blocks_total);
        cov.push_back(s.r.coverage_checksum);
        pow.push_back(s.r.pow_checksum);
        r.max_exceeds_u128 = r.max_exceeds_u128 || s.r.max_exceeds_u128;
        saturated_unknown =
            saturated_unknown || (s.r.max_exceeds_u128 && s.r.max_excursion_wide.empty());
        r.records_truncated = r.records_truncated || s.r.records_truncated;
        // Max with the smallest-n tie-break (Result contract), compared as exact
        // decimals so a slice above 2^128 (max_excursion_wide) orders right.
        const bool first = (&s == &slices[0]);
        const std::string dec = verify::max_excursion_decimal(s.r);
        if (first || decimal_less(max_dec, dec) ||
            (dec == max_dec && s.r.max_excursion_n < r.max_excursion_n)) {
            max_dec = dec;
            r.max_excursion = s.r.max_excursion;
            r.max_excursion_n = s.r.max_excursion_n;
            r.max_excursion_wide = s.r.max_excursion_wide;
        }
        hits.insert(hits.end(), s.r.records.begin(), s.r.records.end());
        r.seconds_total += s.r.seconds_total;
        r.seconds_step1_2 += s.r.seconds_step1_2;
        r.seconds_filter += s.r.seconds_filter;
        r.bv_cache_hit = first ? s.r.bv_cache_hit : (r.bv_cache_hit && s.r.bv_cache_hit);
    }
    r.records = verify::merge_records(std::move(hits));
    // Each slice kept its kRecordsCap smallest n, so the union's smallest are
    // among them: the merged list equals the unsliced run's.
    if (r.records.size() > verify::kRecordsCap) {
        r.records.resize(verify::kRecordsCap);
        r.records_truncated = true;
    }
    const u128 pre_total = static_cast<u128>(km1) * pre.preflight_candidates;
    if (cand < pre_total || cand - pre_total > ~std::uint64_t{0}) {
        err = "candidates_checked sum is inconsistent with the preflight count";
        return false;
    }
    r.candidates_checked = static_cast<std::uint64_t>(cand - pre_total);
    r.step3_steps_total = merge_wrap64(steps, pre.preflight_step3_steps);
    r.blocks_total = merge_wrap64(blocks, pre.preflight_blocks);
    r.coverage_checksum = merge_mod_p(cov, pre.preflight_coverage);
    r.pow_checksum = merge_mod_p(pow, pre.preflight_pow);
    r.pow_available = pre.pow_available;
    r.pow_kernel_id = pre.pow_kernel_id;
    r.preflight_candidates = pre.preflight_candidates;
    r.preflight_coverage = pre.preflight_coverage;
    r.preflight_pow = pre.preflight_pow;
    r.preflight_blocks = pre.preflight_blocks;
    r.preflight_step3_steps = pre.preflight_step3_steps;
    // Exactness is a property of the global record vs the run's bound, so
    // recompute it (a slice's own flag compares only its local record).
    r.max_exact = verify::result_max_exact(s0.N, s0.block_h, s0.unwalked_depth, r);
    if (saturated_unknown) {
        r.max_exact = false; // some slice's maximum is unknown (see above)
    }
    if (info != nullptr) {
        info->depth = s0.unwalked_depth;
        info->max_unknown = saturated_unknown;
        info->backend = backend_label(slices);
        info->commit = s0.commit; // check_slices_match: every slice has it
    }
    out = std::move(r);
    return true;
}

} // namespace collatz::checkpoint
