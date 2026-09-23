// Frontier block mode, property tier: the case-slice merge of a block, a block
// holding a 128-bit overflow, the record list and its cap, for block runs of
// verify::run (models in tests/support/frontier_model.hpp).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "frontier_model.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <string>
#include <vector>

namespace v = collatz::verify;
namespace ckp = collatz::checkpoint;
using collatz::u128;
using namespace collatz::frontier_test;

TEST_CASE("frontier: case slices of a block merge to the unsliced block exactly") {
    const unsigned K = 26;
    const u128 H = pow2(50) + 1; // values above 2^64: the u128 candidate path
    v::Options o = shipped(K, H, 16);
    const v::Result full0 = v::run(o);
    // A threshold a few bits below the max, so the record list is non-trivial.
    mpz_class thr(v::max_excursion_decimal(full0), 10);
    thr >>= 3;
    o.record_threshold = collatz::oracle::to_decimal(thr);
    const v::Result full = v::run(o);
    REQUIRE(full.records.size() >= 1);
    std::vector<ckp::Slice> slices;
    for (std::uint64_t c = 0; c < 4; ++c) {
        v::Options so = o;
        so.step1.case_c = c;
        so.step1.case_k = 4;
        const v::Result sr = v::run(so);
        CHECK(sr.preflight_candidates == 0);
        ckp::Slice s;
        std::string err;
        REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(ckp::JsonMeta{}, so, sr), s, err), err);
        slices.push_back(s);
    }
    v::Result m;
    std::string err;
    ckp::MergeInfo info;
    REQUIRE_MESSAGE(ckp::merge(slices, m, err, &info), err);
    CHECK(m.candidates_checked == full.candidates_checked);
    CHECK(m.step2_masked_out == full.step2_masked_out);
    CHECK(m.step3_steps_total == full.step3_steps_total);
    CHECK(m.blocks_total == full.blocks_total);
    CHECK(m.coverage_checksum == full.coverage_checksum);
    CHECK(m.pow_checksum == full.pow_checksum);
    CHECK(m.max_excursion == full.max_excursion);
    CHECK(m.max_excursion_n == full.max_excursion_n);
    CHECK(m.max_exact == full.max_exact);
    CHECK(m.overflow_fallbacks == full.overflow_fallbacks);
    CHECK(m.step1.survivors == full.step1.survivors);
    REQUIRE(m.records.size() == full.records.size());
    for (std::size_t i = 0; i < m.records.size(); ++i) {
        CHECK(m.records[i].n == full.records[i].n);
        CHECK(m.records[i].peak == full.records[i].peak);
    }
    // One string compares the merged unit with the unsliced one.
    CHECK(ckp::result_digest(o, info.depth, m) ==
          ckp::result_digest(o, v::unwalked_depth(o), full));
}

TEST_CASE("frontier: a block holding a 128-bit overflow reports the exact wide max") {
    // 274133054632352106267 (from hellpig's verifier, github.com/hellpig/collatz)
    // climbs to about 2^135.4.
    u128 n_hp = 0;
    REQUIRE(collatz::from_string("274133054632352106267", n_hp));
    const unsigned K = 24;
    const u128 H = n_hp >> K;
    v::Options o = plain(K, H, 16);
    o.record_threshold = collatz::to_string(collatz::u128_max);
    const v::Result r = v::run(o);
    CHECK(r.overflow_fallbacks > 0);
    CHECK(r.max_exceeds_u128);
    CHECK(r.max_excursion == collatz::u128_max);
    REQUIRE_FALSE(r.max_excursion_wide.empty());
    CHECK(r.max_excursion_n == n_hp);
    // GMP walk of the argmax, as the pipeline defines its excursion.
    const Walk w = walk_candidate(n_hp, K - o.A);
    CHECK(w.wide);
    CHECK(r.max_excursion_wide == w.peak);
    CHECK(r.max_excursion_wide == "56649062372194325899121269007146717645316");
    // The record list holds every walked n above 2^128 - 1.
    REQUIRE(r.records.size() >= 1);
    bool found = false;
    for (const v::RecordHit& h : r.records) {
        CHECK(h.peak.size() >= 39);
        found = found || (h.n == n_hp && h.peak == w.peak);
    }
    CHECK(found);
    CHECK(r.max_exact); // 2^135.4 against (H+1)*2^24*(3/2)^30 ~ 2^85.6
    const Model md = block_model(K, H, o.A, o.B, o.num_bitvectors);
    check_against_model(r, md);
    CHECK(md.wide >= 1);
    (void) check_shipped_vs_model(K, H, o.B, md);
    // Thread count never changes the wide record.
    v::Options o1 = o;
    o1.threads = 1;
    const v::Result r1 = v::run(o1);
    CHECK(r1.max_excursion_wide == r.max_excursion_wide);
    CHECK(r1.max_excursion_n == r.max_excursion_n);
    CHECK(r1.records.size() == r.records.size());
}

TEST_CASE("frontier: the record list is every walked n above the threshold") {
    struct Case {
        unsigned K;
        u128 H;
    };
    const Case cases[] = {{22, 12345}, {22, pow2(50) + 1}, {24, static_cast<u128>(3) << 40}};
    for (const Case& c : cases) {
        CAPTURE(collatz::to_string(c.H));
        v::Options o = plain(c.K, c.H, 16);
        const Model md = block_model(c.K, c.H, o.A, o.B, o.num_bitvectors);
        REQUIRE(md.peaks.size() > 20);
        (void) check_shipped_vs_model(c.K, c.H, o.B, md);
        // About the top 1% of walked peaks exceed X.
        std::vector<std::string> ps;
        ps.reserve(md.peaks.size());
        for (const auto& p : md.peaks) {
            ps.push_back(p.second);
        }
        std::sort(ps.begin(), ps.end(), [](const std::string& a, const std::string& b) {
            return collatz::decimal_less(a, b);
        });
        const std::string X = ps[ps.size() - 1 - ps.size() / 100];
        o.record_threshold = X;
        const v::Result r = v::run(o);
        std::vector<std::pair<u128, std::string>> want;
        for (const auto& p : md.peaks) {
            if (collatz::decimal_less(X, p.second)) {
                want.push_back(p);
            }
        }
        REQUIRE(r.records.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CHECK(r.records[i].n == want[i].first);
            CHECK(r.records[i].peak == want[i].second);
        }
        // A threshold just below the max holds the argmax; at the max, nothing.
        mpz_class below(v::max_excursion_decimal(r), 10);
        below -= 1;
        o.record_threshold = collatz::oracle::to_decimal(below);
        const v::Result r2 = v::run(o);
        REQUIRE(r2.records.size() >= 1);
        CHECK(r2.records.front().n <= r2.max_excursion_n);
        bool has_argmax = false;
        for (const v::RecordHit& h : r2.records) {
            has_argmax =
                has_argmax || (h.n == r2.max_excursion_n && h.peak == v::max_excursion_decimal(r2));
        }
        CHECK(has_argmax);
        o.record_threshold = v::max_excursion_decimal(r);
        CHECK(v::run(o).records.empty());
        // The list never changes any other result field.
        CHECK(r.coverage_checksum == r2.coverage_checksum);
        CHECK(r.pow_checksum == r2.pow_checksum);
        CHECK(r.max_excursion_n == r2.max_excursion_n);
    }
}

TEST_CASE("frontier: the record cap keeps the smallest n whatever the thread count") {
    const unsigned K = 22;
    const u128 H = pow2(50) + 1;
    v::Options o = plain(K, H, 16);
    o.record_threshold = "0"; // every walked candidate is a hit
    const v::Result full = v::run(o);
    REQUIRE(full.records.size() == full.candidates_checked);
    CHECK_FALSE(full.records_truncated);
    for (const std::size_t cap : {std::size_t{1}, std::size_t{7}, std::size_t{1000}}) {
        for (const unsigned threads : {1u, 4u}) {
            CAPTURE(cap);
            CAPTURE(threads);
            v::Options oc = o;
            oc.records_cap = cap;
            oc.threads = threads;
            const v::Result r = v::run(oc);
            CHECK(r.records_truncated);
            REQUIRE(r.records.size() == cap);
            for (std::size_t i = 0; i < cap; ++i) {
                CHECK(r.records[i].n == full.records[i].n);
                CHECK(r.records[i].peak == full.records[i].peak);
            }
            // The cap changes no other field, but the result digest covers the records.
            CHECK(r.coverage_checksum == full.coverage_checksum);
            CHECK(r.pow_checksum == full.pow_checksum);
            CHECK(r.max_excursion_n == full.max_excursion_n);
            CHECK(ckp::result_digest(oc, v::unwalked_depth(oc), r) !=
                  ckp::result_digest(o, v::unwalked_depth(o), full));
        }
    }
    v::Options exact = o;
    exact.records_cap = full.records.size(); // exactly enough room
    const v::Result r = v::run(exact);
    CHECK_FALSE(r.records_truncated);
    CHECK(r.records.size() == full.records.size());
}
