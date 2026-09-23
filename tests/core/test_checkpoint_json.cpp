// The `verify --json` document: the JSON writer/parser round trip, and for
// block mode the round trip of the wide max and record list, and the result
// digest, which parse_slice recomputes for every document the writer produces.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "block_mode.hpp"
#include "checkpoint_helpers.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <optional>
#include <random>
#include <string>

namespace ckp = collatz::checkpoint;
namespace v = collatz::verify;
using collatz::u128;
using collatz::test::block_opts;
using collatz::test::small_opts;

TEST_CASE("result_json round-trips through parse_slice; truncated JSON is rejected") {
    collatz::verify::Options o = small_opts(30);
    o.step1.case_c = 5;
    o.step1.case_k = 8;
    collatz::verify::Result r;
    r.candidates_checked = 18446744073709551615ull;
    r.coverage_checksum = ckp::kP61 - 1;
    r.pow_checksum = 42;
    r.blocks_total = 9;
    r.step3_steps_total = 9;
    r.pow_available = true;
    r.pow_kernel_id = 16;
    r.max_excursion = (static_cast<u128>(1) << 100) + 7;
    r.max_excursion_n = (static_cast<u128>(1) << 70) + 1;
    r.step1.survivors = 123;
    r.preflight_candidates = 131071;
    r.preflight_coverage = 1;
    r.preflight_pow = 2;
    r.preflight_blocks = 3;
    r.preflight_step3_steps = 4;
    ckp::JsonMeta meta;
    const std::string doc =
        ckp::result_json(meta, o, r, "\"checkpoint\": {\"unwalked_depth\": 47}");
    // params.unwalked_depth (the exact verify::unwalked_depth) wins over the
    // checkpoint block's copy.
    REQUIRE(doc.find("\"unwalked_depth\": " + std::to_string(collatz::verify::unwalked_depth(o))) !=
            std::string::npos);
    ckp::Slice s;
    std::string err;
    REQUIRE_MESSAGE(ckp::parse_slice(doc, s, err), err);
    CHECK(s.c == 5);
    CHECK(s.k == 8);
    CHECK(s.N == 30);
    CHECK(s.unwalked_depth == collatz::verify::unwalked_depth(o));
    CHECK(s.backend == "cpu");
    CHECK(s.r.candidates_checked == r.candidates_checked);
    CHECK(s.r.coverage_checksum == r.coverage_checksum);
    CHECK(s.r.max_excursion == r.max_excursion);
    CHECK(s.r.max_excursion_n == r.max_excursion_n);
    CHECK(s.r.preflight_step3_steps == 4);
    CHECK(s.r.step1.survivors == 123);
    for (const std::size_t cut : {doc.size() - 2, doc.size() / 2, std::size_t{10}}) {
        ckp::Slice t;
        CHECK_FALSE(ckp::parse_slice(doc.substr(0, cut), t, err));
    }
}

TEST_CASE("block JSON round-trips the wide max, records and digest") {
    v::Options o = block_opts(24, 16339603342554);
    o.record_threshold = "340282366920938463463374607431768211455";
    v::Result r;
    r.candidates_checked = 12345;
    r.coverage_checksum = 77;
    r.pow_checksum = 88;
    r.pow_available = true;
    r.pow_kernel_id = 16;
    r.blocks_total = 999;
    r.step3_steps_total = 999;
    r.max_excursion = collatz::u128_max;
    r.max_exceeds_u128 = true;
    r.max_excursion_wide = "56649062372194325899121269007146717645316";
    r.max_excursion_n = static_cast<u128>(274133054632352106ULL) * 1000u + 267u;
    r.max_exact = true;
    r.overflow_fallbacks = 1;
    r.records = {{r.max_excursion_n, r.max_excursion_wide}};
    ckp::JsonMeta meta;
    const std::string doc = ckp::result_json(meta, o, r);
    CHECK(doc.find("\"block\": {\"h\": \"16339603342554\", \"K\": 24, \"L\": "
                   "\"274133054632350449664\", \"end\": \"274133054632367226880\"}") !=
          std::string::npos);
    ckp::Slice s;
    std::string err;
    REQUIRE_MESSAGE(ckp::parse_slice(doc, s, err), err);
    CHECK(s.block_h == o.block_h);
    CHECK(s.r.max_excursion == collatz::u128_max);
    CHECK(s.r.max_excursion_wide == r.max_excursion_wide);
    CHECK(s.r.max_excursion_n == r.max_excursion_n);
    REQUIRE(s.r.records.size() == 1);
    CHECK(s.r.records[0].n == r.max_excursion_n);
    CHECK(s.r.records[0].peak == r.max_excursion_wide);
    // The digest is a function of the documented fields only.
    const std::string d1 = ckp::result_digest(o, v::unwalked_depth(o), r);
    CHECK(doc.find("\"result_digest\": \"" + d1 + "\"") != std::string::npos);
    v::Result r2 = r;
    r2.seconds_total = 123.0;
    r2.prefix_walks = 55;
    v::Options o2 = o;
    o2.threads = 7;
    CHECK(ckp::result_digest(o2, v::unwalked_depth(o2), r2) == d1);
    r2.max_excursion_wide = "56649062372194325899121269007146717645317";
    CHECK(ckp::result_digest(o, v::unwalked_depth(o), r2) != d1);
    o2.block_h += 1;
    CHECK(ckp::result_digest(o2, v::unwalked_depth(o2), r) != d1);
    CHECK(ckp::result_digest_text(o, v::unwalked_depth(o), r)
              .find("max_excursion=56649062372194325899121269007146717645316\n") !=
          std::string::npos);
    // The hypothesis: L in params and in the digest, its source in params only.
    CHECK(doc.find("\"assumes_below\": \"274133054632350449664\", \"assumption_source\": "
                   "\"Barina 2025, all n < 2^71 (doi 10.1007/s11227-025-07337-0)\"") !=
          std::string::npos);
    const std::string text = ckp::result_digest_text(o, v::unwalked_depth(o), r);
    CHECK(text.find("K=24\nassumes_below=274133054632350449664\ncase=") != std::string::npos);
    CHECK(text.find("records_truncated=0\n") != std::string::npos);
    v::Options o3 = o;
    o3.assumption_source = "a \"quoted\"\nsource\x01";
    CHECK(ckp::result_digest(o3, v::unwalked_depth(o3), r) == d1);
    const std::optional<ckp::Json> j3 = ckp::parse_json(ckp::result_json(meta, o3, r));
    REQUIRE(j3.has_value());
    const ckp::Json* src = j3->get("params")->get("assumption_source");
    REQUIRE(src != nullptr);
    CHECK(src->text == o3.assumption_source);
    // records_truncated round-trips and is part of the digest.
    v::Result rt = r;
    rt.records_truncated = true;
    ckp::Slice st;
    REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(meta, o, rt), st, err), err);
    CHECK(st.r.records_truncated);
    CHECK_FALSE(s.r.records_truncated);
    CHECK(ckp::result_digest(o, v::unwalked_depth(o), rt) != d1);
}

TEST_CASE("every document result_json writes passes parse_slice's digest check (property)") {
    // Random options and results across the ranges verify writes: the digest
    // parse_slice recomputes from what it read equals the writer's, so a real
    // slice is never refused, and Slice::opts is the options it was written with.
    std::mt19937_64 rng(20261016);
    const auto u = [&rng](std::uint64_t n) { return rng() % n; };
    const auto u128_of = [&rng] { return (static_cast<u128>(rng()) << 64) | rng(); };
    for (int t = 0; t < 400; ++t) {
        v::Options o;
        o.N = 2 + static_cast<unsigned>(u(95));
        o.A = static_cast<unsigned>(u(std::min(o.N, 15u)));
        o.B = 2 + static_cast<unsigned>(u(31));
        o.num_bitvectors = 1 + static_cast<unsigned>(u(32));
        o.threads = static_cast<unsigned>(u(300));
        o.step1.mod3t = static_cast<unsigned>(u(7));
        o.partial_oee = u(2) != 0;
        o.window_chains = static_cast<unsigned>(u(7));
        o.window_templates_D = static_cast<unsigned>(u(13));
        o.step1.case_k = std::uint64_t{1} << u(11);
        o.step1.case_c = u(o.step1.case_k);
        o.block_h = u(2) != 0 ? 0 : static_cast<u128>(rng() >> 20);
        o.record_threshold = u(2) != 0 ? std::to_string(rng()) : std::string();
        o.assumption_source = u(3) == 0 ? "a \"quoted\"\nsource\x01" : "";
        v::Result r;
        r.candidates_checked = rng();
        r.step2_masked_out = rng();
        r.step3_steps_total = rng();
        r.coverage_checksum = rng() % ckp::kP61;
        r.pow_checksum = rng() % ckp::kP61;
        r.pow_available = u(2) != 0;
        r.pow_kernel_id = 16;
        r.blocks_total = rng();
        r.max_excursion = u128_of();
        r.max_excursion_n = u128_of();
        if (u(4) == 0) {
            r.max_excursion = collatz::u128_max;
            r.max_exceeds_u128 = true;
            if (u(2) != 0) {
                // 2^130 followed by up to three digits: always above 2^128.
                r.max_excursion_wide =
                    "1361129467683753853853498429727072845824" + std::to_string(u(1000));
            }
        }
        r.max_exact = u(2) != 0;
        r.records_truncated = u(2) != 0;
        r.overflow_fallbacks = u(3);
        r.step1.survivors = rng();
        r.preflight_candidates = rng();
        r.preflight_coverage = rng() % ckp::kP61;
        r.preflight_pow = rng() % ckp::kP61;
        r.preflight_blocks = rng();
        r.preflight_step3_steps = rng();
        for (std::uint64_t i = u(4); i > 0; --i) {
            r.records.push_back({u128_of(), collatz::to_string(u128_of())});
        }
        ckp::JsonMeta meta;
        meta.use_gpu = u(2) != 0;
        meta.backend = meta.use_gpu ? "gpu" : "cpu (fallback)";
        if (u(3) == 0) {
            meta.unwalked_depth = static_cast<unsigned>(u(200));
        }
        const unsigned depth = meta.unwalked_depth ? *meta.unwalked_depth : v::unwalked_depth(o);
        ckp::Slice s;
        std::string err;
        CAPTURE(t);
        REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(meta, o, r), s, err), err);
        CHECK(ckp::result_digest(s.opts, s.unwalked_depth, s.r) == ckp::result_digest(o, depth, r));
        CHECK(s.opts.N == o.N);
        CHECK(s.opts.A == o.A);
        CHECK(s.opts.B == o.B);
        CHECK(s.opts.num_bitvectors == o.num_bitvectors);
        CHECK(s.opts.threads == o.threads);
        CHECK(s.opts.block_h == o.block_h);
        CHECK(s.opts.record_threshold == o.record_threshold);
        CHECK(s.opts.step1.case_c == o.step1.case_c);
        CHECK(s.opts.step1.case_k == o.step1.case_k);
        CHECK(v::assumption_source(s.opts) == v::assumption_source(o));
        CHECK(s.gpu == meta.use_gpu);
    }
}
