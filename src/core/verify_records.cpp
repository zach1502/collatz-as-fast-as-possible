#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace collatz::verify {

namespace detail {

namespace {

/// Keep this thread's rt->cap smallest distinct n (prune_record_hits).
[[gnu::noinline]] void prune_records(Acc& acc) {
    prune_record_hits(acc.rec_hits, acc.rec_wide, acc.rt->cap, acc.rec_limit, acc.rec_dropped);
}

/// Add the record hit (n, peak), peak below 2^128, under the cap (see Acc).
inline void push_record(Acc& acc, u128 n, u128 peak) {
    if (n > acc.rec_limit) {
        acc.rec_dropped = true;
        return;
    }
    acc.rec_hits.emplace_back(n, peak);
    if (acc.rec_hits.size() > acc.rt->cap + acc.rt->cap / 2) {
        prune_records(acc);
    }
}

} // namespace

/// Offer an exact excursion above 2^128-1 (decimal) for candidate n: the wide
/// record keeps the largest, smallest n on ties, and it is a record hit when it
/// exceeds the threshold. Cold: only excursions that pass 2^128 reach it.
[[gnu::noinline]] void offer_wide(Acc& acc, u128 n, const std::string& dec) {
    if (acc.wide_v.empty() || decimal_less(acc.wide_v, dec) ||
        (dec == acc.wide_v && n < acc.wide_n)) {
        acc.wide_v = dec;
        acc.wide_n = n;
    }
    if (acc.rt != nullptr && decimal_less(acc.rt->dec, dec)) {
        if (n > acc.rec_limit) {
            acc.rec_dropped = true;
        } else {
            acc.rec_wide.push_back(RecordHit{n, dec});
        }
    }
}

/// Cold record-contender path of finalize_candidate: the rare exact-max
/// reconstruction lives here so finalize_candidate has no address-taken local
/// (it never calls the walks or the oracle directly). That drops the
/// stack-protector canary and keeps it small enough to inline into the flush
/// loop, so the Acc fields stay in registers across candidates. `tail_ge` and
/// `pub` are the values the caller's record gate already computed.
///
/// The value offered is n's excursion up to its first drop below n
/// (step3::first_drop_peak), not the caller's tail_max: the block walk tests
/// "< n" only at block ends, so tail_max can include values reached after the
/// drop. With tail_ge the whole excursion is walked. Otherwise only the prefix
/// bound reached the bar and the M-step prefix is walked: the tail's maximum
/// is below the bar, so the prefix result is the exact excursion whenever it
/// reaches the bar. A tail still in the deferred queue (its tail_max is the
/// lower bound m) is offered again by drain_defer_queue once its walk reaches
/// the bar, and the larger offer wins.
[[gnu::noinline]] void finalize_record_contender(
    Acc& acc, u128 n, bool has_prefix, unsigned M, u128 tail_max, bool tail_ge, u128 pub) {
    if (has_prefix && pub > (tail_ge ? tail_max : static_cast<u128>(0))) {
        ++acc.prefix_walks;
    }
    std::string dec;
    const step3::Outcome q = step3::first_drop_peak(n, &dec, tail_ge ? ~0u : M);
    const u128 fm = q.max_seen;
    if (q.max_exceeds_u128) {
        // fm is u128_max standing for dec, which goes to the wide record.
        acc.max_exceeds = true;
        offer_wide(acc, n, dec);
    } else if (acc.rt != nullptr && acc.rt->fits_u128 && fm > acc.rt->x) {
        // With a record threshold X the bar is at most X + 1, so fm > X has
        // reached the bar and is exact (see above).
        push_record(acc, n, fm);
    }
    if (!acc.have_max || fm > acc.maxv || (fm == acc.maxv && n < acc.maxn)) {
        acc.maxv = fm;
        acc.maxn = n;
        acc.have_max = true;
        acc.bar = fm < acc.bar_cap ? fm : acc.bar_cap;
    }
}

} // namespace detail

void prune_record_hits(std::vector<std::pair<u128, u128>>& hits,
                       std::vector<RecordHit>& wide,
                       std::size_t cap,
                       u128& limit,
                       bool& dropped) {
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    });
    hits.erase(std::unique(hits.begin(),
                           hits.end(),
                           [](const auto& a, const auto& b) { return a.first == b.first; }),
               hits.end());
    if (hits.size() > cap) {
        hits.resize(cap);
        dropped = true;
        limit = hits.back().first;
        const u128 lim = limit;
        wide.erase(std::remove_if(
                       wide.begin(), wide.end(), [lim](const RecordHit& h) { return h.n > lim; }),
                   wide.end());
    }
}

std::vector<RecordHit> finish_records(std::vector<std::pair<u128, u128>> raw,
                                      std::vector<RecordHit> wide,
                                      std::size_t cap,
                                      bool dropped,
                                      bool& truncated) {
    std::sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    });
    raw.erase(std::unique(raw.begin(),
                          raw.end(),
                          [](const auto& a, const auto& b) { return a.first == b.first; }),
              raw.end());
    truncated = dropped;
    // Only the cap smallest u128 hits can be among the cap smallest overall.
    if (raw.size() > cap) {
        raw.resize(cap);
        truncated = true;
    }
    wide.reserve(wide.size() + raw.size());
    for (const auto& [n, peak] : raw) {
        wide.push_back(RecordHit{n, collatz::to_string(peak)});
    }
    std::vector<RecordHit> out = merge_records(std::move(wide));
    if (out.size() > cap) {
        out.resize(cap);
        truncated = true;
    }
    return out;
}

std::vector<RecordHit> merge_records(std::vector<RecordHit> hits) {
    std::sort(hits.begin(), hits.end(), [](const RecordHit& a, const RecordHit& b) {
        return a.n != b.n ? a.n < b.n : decimal_less(b.peak, a.peak); // largest peak first
    });
    std::vector<RecordHit> out;
    out.reserve(hits.size());
    for (RecordHit& h : hits) {
        if (out.empty() || out.back().n != h.n) {
            out.push_back(std::move(h));
        }
    }
    return out;
}

} // namespace collatz::verify
