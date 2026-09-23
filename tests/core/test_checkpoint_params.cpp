// The params of a slice file: the exact unwalked_depth that `verify --json`
// records, the merge's max_exact at that depth, and the params that slices
// without a fingerprint must share.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

namespace ckp = collatz::checkpoint;
using collatz::u128;

namespace {

// `depth` overrides the recorded unwalked_depth (default: verify::unwalked_depth),
// standing in for a file written by a run at a deeper K.
ckp::Slice slice_doc(const collatz::verify::Options& base,
                     std::uint64_t c,
                     std::uint64_t k,
                     u128 maxv,
                     std::optional<unsigned> depth = std::nullopt) {
    collatz::verify::Options o = base;
    o.step1.case_c = c;
    o.step1.case_k = k;
    collatz::verify::Result r;
    r.pow_available = true;
    r.pow_kernel_id = 16;
    r.max_excursion = maxv;
    r.max_excursion_n = 27 + c;
    ckp::JsonMeta meta;
    meta.unwalked_depth = depth;
    const std::string doc = ckp::result_json(meta, o, r);
    ckp::Slice s;
    std::string err;
    REQUIRE_MESSAGE(ckp::parse_slice(doc, s, err), err);
    s.source = "slice" + std::to_string(c);
    return s;
}

} // namespace

TEST_CASE("C1: --json records the exact unwalked_depth") {
    collatz::verify::Options o;
    o.N = 40;
    o.A = 10;
    o.B = 28;
    const ckp::Slice s = slice_doc(o, 0, 1, 1);
    CHECK(s.unwalked_depth == collatz::verify::unwalked_depth(o));
    // A recorded depth deeper than M+B is read back as written.
    const unsigned deep = (o.N - o.A) + o.B + 2u;
    CHECK(slice_doc(o, 0, 1, 1, deep).unwalked_depth == deep);
}

TEST_CASE("C1: merge decides max_exact at the recorded depth, not at M+B") {
    collatz::verify::Options o;
    o.N = 40;
    o.A = 10;
    o.B = 28;
    const unsigned mb = (o.N - o.A) + o.B;
    const unsigned exact = mb + 3u; // the depth a deeper run recorded
    // A record between the two bounds: "yes" at depth M+B, "no" at the true K.
    collatz::verify::Result probe;
    probe.max_excursion = 1;
    while (!collatz::verify::result_max_exact(o.N, 0, mb, probe)) {
        probe.max_excursion <<= 1;
    }
    REQUIRE_FALSE(collatz::verify::result_max_exact(o.N, 0, exact, probe));
    const u128 rec = probe.max_excursion;

    std::string err;
    collatz::verify::Result m;
    ckp::MergeInfo mi;
    std::vector<ckp::Slice> with{slice_doc(o, 0, 2, rec, exact), slice_doc(o, 1, 2, 5, exact)};
    REQUIRE_MESSAGE(ckp::merge(with, m, err, &mi), err);
    CHECK(mi.depth == exact);
    CHECK_FALSE(m.max_exact); // exact depth: correctly "no"
}

TEST_CASE("C1: fingerprint-less slices must share A/B/kernel/params") {
    collatz::verify::Options o;
    o.N = 40;
    o.A = 10;
    o.B = 28;
    std::string err;
    collatz::verify::Result m;
    const auto pair_with = [&](const collatz::verify::Options& o1) {
        return std::vector<ckp::Slice>{slice_doc(o, 0, 2, 9), slice_doc(o1, 1, 2, 9)};
    };
    collatz::verify::Options t = o;
    t.threads = 7; // per-slice, allowed to differ
    CHECK_MESSAGE(ckp::merge(pair_with(t), m, err), err);
    collatz::verify::Options b = o;
    b.B = 30;
    CHECK_FALSE(ckp::merge(pair_with(b), m, err));
    CHECK(err.find("B: 28 vs 30") != std::string::npos);
    collatz::verify::Options a = o;
    a.A = 12;
    CHECK_FALSE(ckp::merge(pair_with(a), m, err));
    CHECK(err.find("A: 10 vs 12") != std::string::npos);
    std::vector<ckp::Slice> dv{slice_doc(o, 0, 2, 9),
                               slice_doc(o, 1, 2, 9, (o.N - o.A) + o.B + 2u)};
    CHECK_FALSE(ckp::merge(dv, m, err));
    CHECK(err.find("unwalked_depth") != std::string::npos);
    // Another params.kernel is refused too.
    std::vector<ckp::Slice> kv{slice_doc(o, 0, 2, 9), slice_doc(o, 1, 2, 9)};
    kv[1].params["kernel"] = "\"other\"";
    CHECK_FALSE(ckp::merge(kv, m, err));
    CHECK(err.find("kernel") != std::string::npos);
}
