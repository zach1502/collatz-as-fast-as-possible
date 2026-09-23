// Step 3 DeferQueue: the candidates finish_block16_n defers, drained by
// drain_deferred, against finish_block16.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/digest_term.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <map>
#include <vector>

using collatz::lo64;
using collatz::u128;
namespace s3 = collatz::step3;

namespace {

// How finish_block16_n resolved each candidate of a run of batches on one queue:
// queued (drained later), folded (its block count and pow term are in the
// batch's fw / fp, summed here) or written to `out`.
struct Fed {
    std::vector<s3::BlockOutcome> out;
    std::vector<char> queued;
    std::vector<char> folded;
    std::uint64_t fw = 0;
    u128 fp = 0;
};

Fed feed(const std::vector<u128>& ns,
         const std::vector<u128>& ms,
         std::size_t bs,
         std::size_t lanes,
         u128 thr,
         s3::DeferQueue& dq) {
    const std::size_t total = ns.size();
    Fed f;
    f.out.resize(total);
    f.queued.assign(total, 0);
    f.folded.assign(total, 0);
    for (std::size_t base = 0; base < total; base += bs) {
        const std::size_t cb = std::min(bs, total - base);
        s3::finish_block16_n(
            ns.data() + base, ms.data() + base, cb, lanes, thr, f.out.data() + base, &dq);
        for (std::size_t l = 0; l < cb; ++l) {
            const bool q = dq.deferred[l] == dq.epoch;
            f.queued[base + l] = q ? 1 : 0;
            f.folded[base + l] = dq.folded && !q ? 1 : 0;
        }
        if (dq.folded) {
            f.fw += dq.fw;
            f.fp += dq.fp;
        }
    }
    return f;
}

// The folded candidates: the sums of finish_block16's blocks and pow terms at
// `thr` equal the driver's fw / fp, and each outcome is max_seen == m with no
// overflow, which the caller reads instead of an out.
void check_folded(const Fed& f,
                  const std::vector<u128>& ns,
                  const std::vector<u128>& ms,
                  u128 thr) {
    std::uint64_t sw = 0;
    u128 sp = 0;
    for (std::size_t i = 0; i < ns.size(); ++i) {
        if (f.folded[i] == 0) {
            continue;
        }
        const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], thr);
        CAPTURE(collatz::to_string(ns[i]));
        REQUIRE(ref.max_seen == ms[i]);
        REQUIRE_FALSE(ref.overflow);
        sw += ref.blocks;
        sp += collatz::digest::result_digest_term(ns[i], ref.blocks, ref.v_end);
    }
    CHECK(f.fw == sw);
    CHECK(collatz::to_string(f.fp) == collatz::to_string(sp));
}

} // namespace

// step3::DeferQueue: candidates the persistent fast lane hands off (u128 /
// above-bar) accumulate across batches and drain in one full-lane group, instead
// of one small slot-driver call per 256-candidate batch. A queued-and-drained
// candidate must yield exactly the BlockOutcome finish_block16 gives, and every
// candidate must be resolved in its batch or queued exactly once (never both,
// never neither); a candidate folded in its batch is checked through the fw / fp
// sums. Covers a mix of resolved/deferred, several batch sizes, thresholds
// (including thr = 0, which disables the fast lane), and the end-of-work drain.
TEST_CASE("finish_block16_n DeferQueue drain == finish_block16 (mixed, cross-batch)") {
    std::vector<u128> ns;
    std::vector<u128> ms;
    // Distinct n (so a drained outcome maps back to one candidate). Group A:
    // small odd starts finishing their whole trajectory (m = n), whose climb
    // defers many under a small bar. Group B: u128 starts (m >= 2^64) that defer
    // at load. The two n ranges are disjoint.
    for (unsigned i = 0; i < 900; ++i) {
        ns.push_back(static_cast<u128>(3u + 2u * i));
        ms.push_back(static_cast<u128>(3u + 2u * i));
    }
    for (unsigned i = 0; i < 40; ++i) {
        ns.push_back(static_cast<u128>(2000003u + 2u * i));
        ms.push_back((static_cast<u128>(1) << 64) + (static_cast<u128>(i) << 20) + 1u);
    }
    const std::size_t total = ns.size();
    std::map<std::uint64_t, std::size_t> nmap; // n < 2^64 and distinct here
    for (std::size_t i = 0; i < total; ++i) {
        nmap[lo64(ns[i])] = i;
    }
    const std::array<u128, 4> thresholds = {static_cast<u128>(0),
                                            static_cast<u128>(1) << 13u,
                                            static_cast<u128>(1) << 20u,
                                            static_cast<u128>(1) << 60u};
    for (const u128 thr : thresholds) {
        std::vector<s3::BlockOutcome> ref(total);
        for (std::size_t i = 0; i < total; ++i) {
            ref[i] = s3::finish_block16(ns[i], ms[i], thr);
        }
        for (const std::size_t lanes : {std::size_t{2}, std::size_t{16}}) {
            for (const std::size_t bs :
                 {std::size_t{1}, std::size_t{255}, std::size_t{256}, std::size_t{257}}) {
                s3::DeferQueue dq;
                const Fed fed = feed(ns, ms, bs, lanes, thr, dq);
                std::vector<s3::BlockOutcome> dout(dq.size());
                s3::drain_deferred(dq, lanes, thr, dout.data());
                std::vector<s3::BlockOutcome> got(total);
                std::vector<char> covered(total, 0);
                for (std::size_t i = 0; i < total; ++i) {
                    if (fed.queued[i] == 0 && fed.folded[i] == 0) {
                        got[i] = fed.out[i];
                        covered[i] = 1;
                    }
                }
                for (std::size_t k = 0; k < dq.size(); ++k) {
                    const std::size_t idx = nmap.at(lo64(dq.n[k]));
                    REQUIRE(fed.queued[idx] == 1);
                    REQUIRE(covered[idx] == 0); // queued exactly once, disjoint from resolved
                    got[idx] = dout[k];
                    covered[idx] = 1;
                }
                CAPTURE(collatz::to_string(thr));
                CAPTURE(lanes);
                CAPTURE(bs);
                check_folded(fed, ns, ms, thr);
                for (std::size_t i = 0; i < total; ++i) {
                    if (fed.folded[i] != 0) {
                        continue; // checked through the fw / fp sums
                    }
                    CAPTURE(collatz::to_string(ns[i]));
                    REQUIRE(covered[i] == 1); // every candidate accounted for
                    REQUIRE(got[i].blocks == ref[i].blocks);
                    REQUIRE(got[i].v_end == ref[i].v_end);
                    REQUIRE(got[i].max_seen == ref[i].max_seen);
                    REQUIRE(got[i].overflow == ref[i].overflow);
                }
            }
        }
    }
}

// Production drains at a later, higher record than the batch saw. With drain
// thr > batch thr, a drained outcome keeps exact blocks/v_end/overflow and a
// max_seen that honours the contract at the drain thr (exact when above it).
TEST_CASE("finish_block16_n DeferQueue drain at a higher threshold than the batch") {
    std::vector<u128> ns;
    std::vector<u128> ms;
    for (unsigned i = 0; i < 900; ++i) {
        ns.push_back(static_cast<u128>(3u + 2u * i));
        ms.push_back(static_cast<u128>(3u + 2u * i));
    }
    for (unsigned i = 0; i < 40; ++i) {
        ns.push_back(static_cast<u128>(2000003u + 2u * i));
        ms.push_back((static_cast<u128>(1) << 64) + (static_cast<u128>(i) << 20) + 1u);
    }
    const std::size_t total = ns.size();
    std::map<std::uint64_t, std::size_t> nmap;
    for (std::size_t i = 0; i < total; ++i) {
        nmap[lo64(ns[i])] = i;
    }
    std::vector<s3::BlockOutcome> exact(total);
    for (std::size_t i = 0; i < total; ++i) {
        exact[i] = s3::finish_block16(ns[i], ms[i], 0);
    }
    const std::array<std::array<u128, 2>, 3> pairs = {{
        {static_cast<u128>(1) << 13u, static_cast<u128>(1) << 20u},
        {static_cast<u128>(1) << 20u, static_cast<u128>(1) << 60u},
        {static_cast<u128>(1) << 13u, static_cast<u128>(1) << 60u},
    }};
    std::size_t drained = 0;
    for (const auto& pr : pairs) {
        const u128 tb = pr[0];
        const u128 td = pr[1];
        for (const std::size_t lanes : {std::size_t{2}, std::size_t{16}}) {
            s3::DeferQueue dq;
            const Fed fed = feed(ns, ms, 256, lanes, tb, dq);
            std::vector<s3::BlockOutcome> dout(dq.size());
            s3::drain_deferred(dq, lanes, td, dout.data());
            check_folded(fed, ns, ms, tb);
            for (std::size_t i = 0; i < total; ++i) {
                if (fed.queued[i] == 0 && fed.folded[i] == 0) {
                    const s3::BlockOutcome ref = s3::finish_block16(ns[i], ms[i], tb);
                    REQUIRE(fed.out[i].blocks == ref.blocks);
                    REQUIRE(fed.out[i].v_end == ref.v_end);
                    REQUIRE(fed.out[i].max_seen == ref.max_seen);
                    REQUIRE(fed.out[i].overflow == ref.overflow);
                }
            }
            for (std::size_t k = 0; k < dq.size(); ++k) {
                const std::size_t i = nmap.at(lo64(dq.n[k]));
                CAPTURE(collatz::to_string(tb));
                CAPTURE(collatz::to_string(td));
                CAPTURE(lanes);
                CAPTURE(collatz::to_string(ns[i]));
                REQUIRE(fed.queued[i] == 1);
                REQUIRE(dout[k].blocks == exact[i].blocks);
                REQUIRE(dout[k].v_end == exact[i].v_end);
                REQUIRE(dout[k].overflow == exact[i].overflow);
                if (exact[i].max_seen > td) {
                    REQUIRE(dout[k].max_seen == exact[i].max_seen);
                } else {
                    REQUIRE(dout[k].max_seen <= td);
                    REQUIRE(dout[k].max_seen <= exact[i].max_seen);
                }
                ++drained;
            }
        }
    }
    REQUIRE(drained > 0);
}

// Exact queue sizes 1 / 255 / 256 / 257 (the last spanning two feed batches so
// the queue crosses the 256 threshold) plus the drain-of-all: every queued
// candidate's drained outcome equals finish_block16 of its own (n, m).
TEST_CASE("finish_block16_n DeferQueue exact sizes 1/255/256/257 drain") {
    const u128 thr = static_cast<u128>(1) << 20u; // hi = 2^10 > 3 so the fast lane is active
    for (const std::size_t S :
         {std::size_t{1}, std::size_t{255}, std::size_t{256}, std::size_t{257}}) {
        std::vector<u128> ns;
        std::vector<u128> ms;
        for (std::size_t i = 0; i < S; ++i) {
            ns.push_back(static_cast<u128>(100003u + 2u * i));
            ms.push_back((static_cast<u128>(1) << 64) + (static_cast<u128>(i) << 20) + 1u);
        }
        s3::DeferQueue dq;
        std::vector<s3::BlockOutcome> out(S);
        for (std::size_t base = 0; base < S; base += 256) {
            const std::size_t cb = std::min(std::size_t{256}, S - base);
            const std::size_t sz0 = dq.size();
            s3::finish_block16_n(
                ns.data() + base, ms.data() + base, cb, 16, thr, out.data() + base, &dq);
            REQUIRE(dq.size() - sz0 == cb); // every u128 start defers at load
        }
        REQUIRE(dq.size() == S);
        std::vector<s3::BlockOutcome> dout(S);
        s3::drain_deferred(dq, 16, thr, dout.data());
        for (std::size_t k = 0; k < S; ++k) {
            const s3::BlockOutcome ref = s3::finish_block16(dq.n[k], dq.m[k], thr);
            CAPTURE(S);
            CAPTURE(collatz::to_string(dq.n[k]));
            REQUIRE(dout[k].blocks == ref.blocks);
            REQUIRE(dout[k].v_end == ref.v_end);
            REQUIRE(dout[k].max_seen == ref.max_seen);
            REQUIRE(dout[k].overflow == ref.overflow);
        }
    }
}
