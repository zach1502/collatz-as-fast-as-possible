// The exact slice merge (`collatz merge`): the merge arithmetic (u128 sums,
// mod 2^61-1, wrapping u64, preflight subtraction, max tie-break), coverage
// gap/overlap detection, fingerprint mismatch refusal, the reported backend,
// and for block mode the merge of saturated and wide maxima.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "block_mode.hpp"
#include "checkpoint_helpers.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <doctest/doctest.h>
#include <random>
#include <string>
#include <vector>

namespace ckp = collatz::checkpoint;
namespace v = collatz::verify;
using collatz::u128;
using collatz::test::block_opts;
using collatz::test::check_same;

namespace {

// Exact reference for (sum - (k-1)*pre) mod m in u128 arithmetic: values are
// below 2^64 and k <= 2^16, so no sum or product overflows.
std::uint64_t ref_mod(const std::vector<std::uint64_t>& v, std::uint64_t pre, std::uint64_t m) {
    u128 s = 0;
    for (const auto x : v) {
        s += x;
    }
    const u128 sub = static_cast<u128>(v.size() - 1) * pre;
    // s - sub may be negative, so a multiple of m above sub is added first.
    const u128 big = (sub / m + 1) * m;
    return static_cast<std::uint64_t>((s + big - sub) % m);
}

ckp::Slice mk(std::uint64_t c, std::uint64_t k, u128 maxv, u128 maxn) {
    ckp::Slice s;
    s.c = c;
    s.k = k;
    s.N = 40;
    s.unwalked_depth = 58;
    s.backend = "cpu";
    s.r.max_excursion = maxv;
    s.r.max_excursion_n = maxn;
    s.r.pow_available = true;
    s.r.pow_kernel_id = 16;
    s.source = "s" + std::to_string(c);
    return s;
}

} // namespace

TEST_CASE("merge_mod_p / merge_wrap64 match an exact reference (property)") {
    std::mt19937_64 rng(12345);
    for (int trial = 0; trial < 2000; ++trial) {
        const std::size_t k = 1 + rng() % 1024;
        std::vector<std::uint64_t> modp(k);
        std::vector<std::uint64_t> raw(k);
        for (std::size_t i = 0; i < k; ++i) {
            modp[i] = rng() % ckp::kP61; // slices store reduced values
            raw[i] = rng();              // wrapping u64 partials
        }
        // Edge-heavy preflight values.
        const std::uint64_t pre_choices[] = {0, 1, ckp::kP61 - 1, rng() % ckp::kP61};
        const std::uint64_t pre = pre_choices[trial % 4];
        CHECK(ckp::merge_mod_p(modp, pre) == ref_mod(modp, pre, ckp::kP61));
        std::uint64_t w = 0;
        for (const auto x : raw) {
            w += x;
        }
        const std::uint64_t pre64 = rng();
        CHECK(ckp::merge_wrap64(raw, pre64) == w - static_cast<std::uint64_t>(k - 1) * pre64);
    }
    // k == 1 is the identity (nothing subtracted).
    CHECK(ckp::merge_mod_p({ckp::kP61 - 1}, 7) == ckp::kP61 - 1);
    CHECK(ckp::merge_wrap64({~std::uint64_t{0}}, 7) == ~std::uint64_t{0});
    // The result is always reduced, even if every slice equals the preflight.
    CHECK(ckp::merge_mod_p({5, 5, 5, 5}, 5) == 5);
}

TEST_CASE("merge: preflight subtraction, u128 candidate sum, max tie-break, OR") {
    const std::uint64_t K = 4;
    std::vector<ckp::Slice> v;
    const std::uint64_t big = (std::uint64_t{1} << 62) + 3; // sum of 4 overflows u64
    for (std::uint64_t c = 0; c < K; ++c) {
        ckp::Slice s = mk(c, K, 1000, 50 + c);
        s.r.preflight_candidates = big - 10;
        s.r.candidates_checked = big + c; // = pre + (10 + c)
        s.r.preflight_coverage = ckp::kP61 - 2;
        s.r.coverage_checksum = (ckp::kP61 - 2 + c) % ckp::kP61;
        s.r.preflight_pow = 3;
        s.r.pow_checksum = 3 + c;
        s.r.preflight_blocks = 100;
        s.r.blocks_total = 100 + c;
        s.r.preflight_step3_steps = 200;
        s.r.step3_steps_total = 200 + 2 * c;
        s.r.step2_masked_out = 7;
        s.r.step1.survivors = 11;
        s.r.overflow_fallbacks = c == 2 ? 1 : 0;
        v.push_back(s);
    }
    // Slice 3 ties slice 2's max with a smaller n; slice 1 has a smaller max.
    v[1].r.max_excursion = 999;
    v[1].r.max_excursion_n = 1;
    v[2].r.max_excursion = 5000;
    v[2].r.max_excursion_n = 77;
    v[3].r.max_excursion = 5000;
    v[3].r.max_excursion_n = 66;
    v[0].r.max_exceeds_u128 = false;
    v[3].r.max_exceeds_u128 = true;
    collatz::verify::Result m;
    std::string err;
    REQUIRE(ckp::merge(v, m, err));
    CHECK(m.candidates_checked == (big - 10) + 10 + 11 + 12 + 13);
    CHECK(m.coverage_checksum == (ckp::kP61 - 2 + 0 + 1 + 2 + 3) % ckp::kP61);
    CHECK(m.pow_checksum == 3 + 6);
    CHECK(m.blocks_total == 100 + 6);
    CHECK(m.step3_steps_total == 200 + 12);
    CHECK(m.step2_masked_out == 28);
    CHECK(m.step1.survivors == 44);
    CHECK(m.overflow_fallbacks == 1);
    CHECK(m.max_excursion == 5000);
    CHECK(m.max_excursion_n == 66); // smallest n on ties
    CHECK(m.max_exceeds_u128);
    CHECK_FALSE(m.max_exact); // exceeds_u128 forces false
    CHECK(m.preflight_candidates == big - 10);
    // Order independence: reversing the slice list changes nothing.
    std::vector<ckp::Slice> rev(v.rbegin(), v.rend());
    collatz::verify::Result m2;
    REQUIRE(ckp::merge(rev, m2, err));
    check_same(m, m2);
}

TEST_CASE("merge refuses gaps, overlaps, mixed K, mismatched preflight or fingerprint") {
    std::string err;
    collatz::verify::Result m;
    std::vector<ckp::Slice> v{mk(0, 4, 1, 1), mk(1, 4, 1, 1), mk(3, 4, 1, 1)};
    CHECK_FALSE(ckp::merge(v, m, err));
    CHECK(err.find("missing (1): 2") != std::string::npos);
    std::vector<std::uint64_t> miss;
    std::vector<std::uint64_t> dup;
    ckp::coverage(v, 4, miss, dup);
    CHECK(miss == std::vector<std::uint64_t>{2});
    CHECK(dup.empty());

    v.push_back(mk(1, 4, 1, 1));
    CHECK_FALSE(ckp::merge(v, m, err));
    CHECK(err.find("duplicated: 1") != std::string::npos);

    std::vector<ckp::Slice> mixed{mk(0, 2, 1, 1), mk(1, 4, 1, 1)};
    CHECK_FALSE(ckp::merge(mixed, m, err));

    std::vector<ckp::Slice> pre{mk(0, 2, 1, 1), mk(1, 2, 1, 1)};
    pre[1].r.preflight_pow = 9;
    CHECK_FALSE(ckp::merge(pre, m, err));
    CHECK(err.find("preflight") != std::string::npos);

    std::vector<ckp::Slice> fpm{mk(0, 2, 1, 1), mk(1, 2, 1, 1)};
    fpm[0].fingerprint["B"] = "30";
    fpm[1].fingerprint["B"] = "31";
    CHECK_FALSE(ckp::merge(fpm, m, err));
    CHECK(err.find("B: '30'") != std::string::npos);
}

TEST_CASE("merge reports the slices' actual backend") {
    std::string err;
    collatz::verify::Result m;
    ckp::MergeInfo mi;
    std::vector<ckp::Slice> v{mk(0, 2, 1, 1), mk(1, 2, 1, 1)};
    v[0].backend = v[1].backend = "gpu";
    REQUIRE(ckp::merge(v, m, err, &mi));
    CHECK(mi.backend == "gpu");
    v[1].backend = "cpu (fallback)";
    REQUIRE(ckp::merge(v, m, err, &mi));
    CHECK(mi.backend == "mixed: gpu + cpu (fallback)");
}

TEST_CASE("merge: a slice saturated above 2^128 without its exact value forces max_exact no") {
    const v::Options o = block_opts(24, 9);
    const auto make = [&](std::uint64_t c, bool wide) {
        v::Options so = o;
        so.step1.case_c = c;
        so.step1.case_k = 2;
        v::Result r;
        r.pow_available = true;
        r.pow_kernel_id = 16;
        r.candidates_checked = 10;
        r.max_excursion = collatz::u128_max;
        r.max_exceeds_u128 = true;
        r.max_excursion_n = 900 + c;
        if (wide) {
            r.max_excursion_wide = "1361129467683753853853498429727072845824"; // 2^130
        }
        ckp::Slice s;
        std::string err;
        REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(ckp::JsonMeta{}, so, r), s, err), err);
        return s;
    };
    v::Result m;
    std::string err;
    ckp::MergeInfo info;
    // Both slices exact: 2^130 is far above the block bound, so max_exact holds.
    REQUIRE_MESSAGE(ckp::merge({make(0, true), make(1, true)}, m, err, &info), err);
    CHECK(m.max_exact);
    CHECK_FALSE(info.max_unknown);
    // Slice 1's maximum passed 2^128 but its value is unknown (a GPU slice):
    // it could exceed slice 0's 2^130, so the merged max is not exact.
    REQUIRE_MESSAGE(ckp::merge({make(0, true), make(1, false)}, m, err, &info), err);
    CHECK(m.max_excursion_wide == "1361129467683753853853498429727072845824");
    CHECK_FALSE(m.max_exact);
    CHECK(info.max_unknown);
}

TEST_CASE("merge orders slices by the exact max, wide values included") {
    v::Options o = block_opts(24, 9);
    std::vector<ckp::Slice> sl;
    for (std::uint64_t c = 0; c < 2; ++c) {
        v::Options so = o;
        so.step1.case_c = c;
        so.step1.case_k = 2;
        v::Result r;
        r.pow_available = true;
        r.pow_kernel_id = 16;
        r.candidates_checked = 10 + c;
        if (c == 0) {
            r.max_excursion = collatz::u128_max; // stands for 2^130
            r.max_exceeds_u128 = true;
            r.max_excursion_wide = "1361129467683753853853498429727072845824";
            r.max_excursion_n = 900;
            r.records = {{900, r.max_excursion_wide}};
        } else {
            r.max_excursion = collatz::u128_max - 5; // larger as u128, smaller as a value
            r.max_excursion_n = 100;
            r.records = {{100, collatz::to_string(r.max_excursion)}};
        }
        ckp::Slice s;
        std::string err;
        REQUIRE_MESSAGE(ckp::parse_slice(ckp::result_json(ckp::JsonMeta{}, so, r), s, err), err);
        sl.push_back(s);
    }
    v::Result m;
    std::string err;
    REQUIRE_MESSAGE(ckp::merge(sl, m, err), err);
    CHECK(m.max_excursion_wide == "1361129467683753853853498429727072845824");
    CHECK(m.max_excursion_n == 900);
    CHECK(m.max_exceeds_u128);
    REQUIRE(m.records.size() == 2);
    CHECK(m.records[0].n == 100);
    CHECK(m.records[1].n == 900);
    CHECK(m.preflight_candidates == 0);
    CHECK(m.candidates_checked == 21);
    // Slices of different blocks never merge.
    v::Options other = o;
    other.block_h = 10;
    other.step1.case_c = 1;
    other.step1.case_k = 2;
    ckp::Slice bad;
    REQUIRE(ckp::parse_slice(ckp::result_json(ckp::JsonMeta{}, other, sl[1].r), bad, err));
    std::vector<ckp::Slice> mixed = {sl[0], bad};
    CHECK_FALSE(ckp::merge(mixed, m, err));
    CHECK(err.find("block") != std::string::npos);
}
