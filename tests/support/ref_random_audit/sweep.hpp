#ifndef COLLATZ_TESTS_REF_RANDOM_AUDIT_SWEEP_HPP
#define COLLATZ_TESTS_REF_RANDOM_AUDIT_SWEEP_HPP

// The runs of the ref/ audit: one production run per bitvector-cache mode,
// compared with ref/ (run_and_compare), and the seeded sweep over drawn
// configurations. ref_random_audit.hpp includes this file.

#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "collatz_ref/ref_verify.hpp"
#include "ref_random_audit/draw.hpp"
#include "test_env.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <optional>
#include <process.h>
#include <sstream>
#include <string>
#include <system_error>

#define COLLATZ_AUDIT_GETPID _getpid

namespace collatz::test::audit {

/// Saves and restores one environment variable around a scope.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) {
            old_ = std::string(v);
        }
    }
    void set(const char* v) {
        set_env(name_, v);
    }
    void unset() {
        unset_env(name_);
    }
    ~EnvGuard() {
        if (old_) {
            set_env(name_, old_->c_str());
        } else {
            unset_env(name_);
        }
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

enum class Mode { cold, cold_write, warm, shared };

inline const char* mode_name(Mode m) {
    switch (m) {
    case Mode::cold:
        return "cold(bv_cache=false)";
    case Mode::cold_write:
        return "cold-write(fresh dir)";
    case Mode::warm:
        return "warm(fresh dir, 2nd run)";
    case Mode::shared:
        return "shared(sweep-wide dir)";
    }
    return "?";
}

/// Run production once in `mode` and compare every scalar to `ref`. Returns
/// false (after reporting) on any mismatch.
inline bool run_and_compare(const Cfg& c,
                            const ref::VerifyResult& ref,
                            bool exact_expected,
                            Mode mode,
                            const std::filesystem::path& dir,
                            const std::string& tag) {
    EnvGuard g_dir("COLLATZ_CACHE_DIR");
    verify::Options o = make_options(c);
    if (mode == Mode::cold) {
        o.bv_cache = false;
        g_dir.unset();
    } else {
        o.bv_cache = true;
        g_dir.set(dir.string().c_str());
    }
    const verify::Result p = verify::run(o);

    bool ok = p.candidates_checked == ref.candidates_checked &&
              p.coverage_checksum == ref.coverage && p.pow_available && p.pow_kernel_id == 16 &&
              p.pow_checksum == ref.pow_v4 && p.blocks_total == ref.blocks_total &&
              p.max_excursion == ref.max_excursion && p.max_excursion_n == ref.max_excursion_n &&
              p.max_exceeds_u128 == ref.max_exceeds_u128 && p.max_exact == exact_expected;
    // Cache-state sanity: a fresh dir must miss, its second run must hit.
    bool cache_ok = true;
    if (mode == Mode::cold || mode == Mode::cold_write) {
        cache_ok = !p.bv_cache_hit;
    } else if (mode == Mode::warm) {
        cache_ok = p.bv_cache_hit;
    }
    if (!ok || !cache_ok) {
        std::ostringstream os;
        os << "AUDIT MISMATCH " << tag << " mode=" << mode_name(mode) << "\n  cfg: " << describe(c)
           << "\n  candidates  prod=" << p.candidates_checked << " ref=" << ref.candidates_checked
           << "\n  coverage    prod=" << p.coverage_checksum << " ref=" << ref.coverage
           << "\n  pow         prod=" << p.pow_checksum << " ref=" << ref.pow_v4
           << " (avail=" << p.pow_available << " kid=" << p.pow_kernel_id << ")"
           << "\n  blocks      prod=" << p.blocks_total << " ref=" << ref.blocks_total
           << "\n  max         prod=" << to_string(p.max_excursion)
           << " ref=" << to_string(ref.max_excursion)
           << "\n  argmax      prod=" << to_string(p.max_excursion_n)
           << " ref=" << to_string(ref.max_excursion_n)
           << "\n  exceeds     prod=" << p.max_exceeds_u128 << " ref=" << ref.max_exceeds_u128
           << "\n  max_exact   prod=" << p.max_exact << " expected=" << exact_expected
           << "\n  bv_cache_hit=" << p.bv_cache_hit << " (cache state ok=" << cache_ok << ")";
        FAIL_CHECK(os.str());
        return false;
    }
    return true;
}

/// Run a sweep of `count` configs from `seed`. COLLATZ_AUDIT_SEED /
/// COLLATZ_AUDIT_CONFIGS / COLLATZ_AUDIT_ONLY override seed, count and select a
/// single index. Returns the number of failing configs.
inline unsigned sweep(std::uint64_t seed, unsigned count, const Tier& tier) {
    if (const char* e = std::getenv("COLLATZ_AUDIT_SEED"); e != nullptr && e[0] != '\0') {
        seed = std::strtoull(e, nullptr, 0);
    }
    if (const char* e = std::getenv("COLLATZ_AUDIT_CONFIGS"); e != nullptr && e[0] != '\0') {
        count = static_cast<unsigned>(std::strtoul(e, nullptr, 0));
    }
    long only = -1;
    if (const char* e = std::getenv("COLLATZ_AUDIT_ONLY"); e != nullptr && e[0] != '\0') {
        only = std::strtol(e, nullptr, 0);
    }
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / ("collatz-audit-ref-" + std::to_string(COLLATZ_AUDIT_GETPID()) +
                                     "-" + std::to_string(seed));
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path shared = root / "shared";
    fs::create_directories(shared, ec);

    unsigned failures = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < count; ++i) {
        if (only >= 0 && static_cast<long>(i) != only) {
            continue;
        }
        const Cfg c = draw(seed, i, tier);
        std::ostringstream tag;
        tag << "[seed=" << seed << " index=" << i << "]";
        ref::Options s1;
        s1.mod3t = c.mod3t;
        const auto tr0 = std::chrono::steady_clock::now();
        const ref::VerifyResult r =
            ref::verify_ref(c.N, c.A, c.B, c.nbv, c.partial_oee, s1, c.tw, c.D);
        const auto tr1 = std::chrono::steady_clock::now();
        const bool exact = expected_max_exact(c, r);

        const fs::path fresh = root / ("cfg" + std::to_string(i));
        fs::create_directories(fresh, ec);
        bool ok = run_and_compare(c, r, exact, Mode::cold, fresh, tag.str());
        ok = run_and_compare(c, r, exact, Mode::cold_write, fresh, tag.str()) && ok;
        ok = run_and_compare(c, r, exact, Mode::warm, fresh, tag.str()) && ok;
        ok = run_and_compare(c, r, exact, Mode::shared, shared, tag.str()) && ok;
        fs::remove_all(fresh, ec);
        const auto tp1 = std::chrono::steady_clock::now();
        if (!ok) {
            ++failures;
        }
        if (std::getenv("COLLATZ_AUDIT_VERBOSE") != nullptr) {
            MESSAGE(tag.str() << (ok ? " ok   " : " FAIL ") << describe(c)
                              << " cand=" << r.candidates_checked << " exact=" << exact
                              << " ref_s=" << std::chrono::duration<double>(tr1 - tr0).count()
                              << " prod4_s=" << std::chrono::duration<double>(tp1 - tr1).count());
        }
    }
    fs::remove_all(root, ec);
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("ref-random audit: seed=" << seed << " configs=" << count << " failures=" << failures
                                      << " seconds=" << secs);
    return failures;
}

} // namespace collatz::test::audit

#endif // COLLATZ_TESTS_REF_RANDOM_AUDIT_SWEEP_HPP
