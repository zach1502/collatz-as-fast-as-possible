#ifndef COLLATZ_TESTS_CERTIFY_AUDIT_HPP
#define COLLATZ_TESTS_CERTIFY_AUDIT_HPP

// The block that tests/integration/test_certify_audit.cpp audits: the K = 32
// block at 2075*2^60, its verify run with the record list, the audit options,
// and the residues a seed draws.

#include "collatz/certify.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "scale.hpp"
#include "test_env.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace collatz::test::certify_audit {

namespace cf = collatz::certify;
namespace v = collatz::verify;
using collatz::u128;

inline constexpr unsigned kK = 32;
inline constexpr unsigned kM = 22; // kK - A, with A = 10
inline const u128 kH = static_cast<u128>(2075) << 28;
// About 1400 of the block's 5.4 million candidates have a larger excursion.
inline constexpr const char* kThreshold = "1000000000000000000000000000";

inline const std::uint64_t kSamples = collatz::test::scaled(8192, 2048);

/// verify's block configuration (A = 10, auto B = 24, mod-3^t 6, partial OEE,
/// window chains 6, templates 12), as `collatz worker` passes it.
inline cf::Options audit_opts(unsigned threads) {
    cf::Options o;
    o.N = kK;
    o.block_h = kH;
    o.A = 10;
    o.B = v::auto_step2_B(kK, false);
    o.num_bitvectors = 8;
    o.threads = threads;
    o.mod3t = 6;
    o.partial_oee = true;
    o.window_chains = 6;
    o.window_templates_D = 12;
    o.bv_cache = collatz::test::scoped_bv_cache();
    return o;
}

/// The block run the audits check, with its record list above kThreshold.
inline const v::Result& block_run() {
    static const v::Result r = [] {
        const cf::Options c = audit_opts(4);
        v::Options o;
        o.N = c.N;
        o.block_h = c.block_h;
        o.A = c.A;
        o.B = c.B;
        o.num_bitvectors = c.num_bitvectors;
        o.threads = 4;
        o.partial_oee = c.partial_oee;
        o.step1.mod3t = c.mod3t;
        o.window_chains = c.window_chains;
        o.window_templates_D = c.window_templates_D;
        o.record_threshold = kThreshold;
        o.bv_cache = c.bv_cache;
        return v::run(o);
    }();
    return r;
}

/// Audit options carrying the block run's reported maximum and record list.
inline cf::AuditOptions reported(const std::string& seed) {
    const v::Result& r = block_run();
    cf::AuditOptions a;
    a.seed = seed;
    a.samples = kSamples;
    a.max_excursion = v::max_excursion_decimal(r);
    a.record_threshold = kThreshold;
    for (const v::RecordHit& h : r.records) {
        a.records.push_back(cf::ReportedRecord{h.n, h.peak});
    }
    a.records_truncated = r.records_truncated;
    return a;
}

inline std::set<u128> drawn(const std::string& seed, std::uint64_t samples) {
    std::set<u128> s;
    for (std::uint64_t i = 0; i < samples; ++i) {
        s.insert(cf::audit_residue(kH, kK, kM, seed, i));
    }
    return s;
}

/// A seed whose sample draws the residue of at least one listed record holder,
/// a walked candidate with a large excursion, and the drawn holders.
inline std::pair<std::string, std::vector<v::RecordHit>> seed_drawing_records() {
    const u128 low = (static_cast<u128>(1) << kM) - 1u;
    for (int t = 1; t <= 64; ++t) {
        const std::string seed = "test-seed-" + std::to_string(t);
        const std::set<u128> d = drawn(seed, kSamples);
        std::vector<v::RecordHit> hits;
        for (const v::RecordHit& h : block_run().records) {
            if (d.count(h.n & low) != 0) {
                hits.push_back(h);
            }
        }
        if (!hits.empty()) {
            return {seed, hits};
        }
    }
    return {};
}

inline void show(const cf::AuditReport& r) {
    for (const cf::Failure& f : r.failures) {
        MESSAGE("FAIL n0=" << f.n0 << " n=" << f.n << " k=" << f.k << " " << f.reason);
    }
}

} // namespace collatz::test::certify_audit

#endif // COLLATZ_TESTS_CERTIFY_AUDIT_HPP
