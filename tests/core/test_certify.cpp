#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/certify.hpp"
#include "collatz/u128.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <stdexcept>
#include <string>
#include <vector>

// The parts of the random block audit that need no Step-2 tables: the seed
// derivation (pinned against an independent SHA-256 computation) and the option
// checks that run before anything is built.

namespace {

namespace cf = collatz::certify;
using collatz::u128;

const u128 kH = static_cast<u128>(2075) << 28;
constexpr const char* kDigest = "f699b5fd2dd9d98a816aa58da0d5fd10ddc19070784cb255e541fccda270618f";

cf::Options block_opts() {
    cf::Options o;
    o.N = 32;
    o.block_h = kH;
    o.A = 10;
    o.B = 24;
    return o;
}

} // namespace

TEST_CASE("audit_residue: SHA-256 of collatz-audit|H|K|S|i, pinned") {
    // Computed with Python's hashlib from the definition in certify.hpp.
    CHECK(cf::audit_residue(kH, 32, 22, "test-seed-1", 0) == 3781339u);
    CHECK(cf::audit_residue(kH, 32, 22, "test-seed-1", 1) == 339583u);
    CHECK(cf::audit_residue(kH, 32, 22, "test-seed-1", 2) == 127671u);
    CHECK(cf::audit_residue(132800, 54, 42, kDigest, 0) == static_cast<u128>(3885483720987ULL));
    CHECK(cf::audit_residue(132800, 54, 42, kDigest, 1) == static_cast<u128>(486569601735ULL));
    CHECK(cf::audit_residue(5, 64, 50, "x", 7) == static_cast<u128>(955574356560223ULL));
}

TEST_CASE("audit_residue: r = 3 (mod 4) below 2^M, and every input matters") {
    for (unsigned M = 3; M <= 65; ++M) {
        for (std::uint64_t i = 0; i < 16; ++i) {
            const u128 r = cf::audit_residue(kH, 40, M, "s", i);
            CHECK((r & 3u) == 3u);
            CHECK(r < (static_cast<u128>(1) << M));
        }
    }
    CHECK_THROWS_AS((void) cf::audit_residue(kH, 40, 2, "s", 0), std::invalid_argument);
    CHECK_THROWS_AS((void) cf::audit_residue(kH, 40, 66, "s", 0), std::invalid_argument);
    const u128 base = cf::audit_residue(kH, 40, 30, "s", 5);
    CHECK(cf::audit_residue(kH + 1, 40, 30, "s", 5) != base);
    CHECK(cf::audit_residue(kH, 41, 30, "s", 5) != base);
    CHECK(cf::audit_residue(kH, 40, 30, "t", 5) != base);
    CHECK(cf::audit_residue(kH, 40, 30, "s", 6) != base);
}

TEST_CASE("audit_default_samples: the large-block default, capped at 1/64 of the residues") {
    CHECK(cf::audit_default_samples(42) == cf::kDefaultAuditSamples);
    CHECK(cf::audit_default_samples(63) == cf::kDefaultAuditSamples);
    CHECK(cf::audit_default_samples(22) == 16384u); // 2^20 / 64
    CHECK(cf::audit_default_samples(10) == 1024u);
    CHECK(cf::audit_default_samples(3) == 1024u);
}

TEST_CASE("audit: options it refuses before building anything") {
    cf::AuditOptions a;
    a.seed = "s";
    cf::Options full = block_opts();
    full.block_h = 0;
    CHECK_THROWS_AS((void) cf::audit(full, a), std::invalid_argument);
    a.samples = cf::kMaxAuditSamples + 1;
    CHECK_THROWS_AS((void) cf::audit(block_opts(), a), std::invalid_argument);
    a.samples = 16;
    a.max_excursion = "12x";
    CHECK_THROWS_AS((void) cf::audit(block_opts(), a), std::invalid_argument);
    a.max_excursion.clear();
    a.record_threshold = "-5";
    CHECK_THROWS_AS((void) cf::audit(block_opts(), a), std::invalid_argument);
    a.record_threshold = "100";
    a.records.push_back(cf::ReportedRecord{kH << 32, "1e9"});
    CHECK_THROWS_AS((void) cf::audit(block_opts(), a), std::invalid_argument);
    a.records.clear();
    // cache_only without the cache: refused when the tables would be built.
    cf::Options c = block_opts();
    c.mod3t = 0;
    c.window_chains = 0;
    c.window_templates_D = 0;
    c.partial_oee = false;
    c.bv_cache = false;
    c.cache_only = true;
    CHECK_THROWS_AS((void) cf::audit(c, a), std::runtime_error);
}

// certify::run() must reject N > 64 at runtime: Phase 1 recomputes the class
// certificates in u128, exact only up to N = 64 (the CLI bound), and Step 1
// only asserts its own bound M <= 76, which NDEBUG compiles out.
TEST_CASE("certify::run rejects N > 64 (u128 certificate bound)") {
    collatz::certify::Options o;
    o.N = 90;
    o.A = 10; // M = 80, past the Step-1 bound as well.
    o.B = 8;
    o.threads = 1;
    CHECK_THROWS_WITH_AS((void) collatz::certify::run(o),
                         doctest::Contains("N must be <= 64"),
                         std::invalid_argument);
    o.N = 65; // M = 55: the first N rejected.
    CHECK_THROWS_WITH_AS((void) collatz::certify::run(o),
                         doctest::Contains("N must be <= 64"),
                         std::invalid_argument);
}
