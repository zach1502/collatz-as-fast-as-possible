#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/linear.hpp"
#include "collatz/oracle.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>

using collatz::u128;
namespace vf = collatz::verify;

namespace {

const unsigned kN = collatz::test::scaled(30, 24);

mpz_class step_T(const mpz_class& x) {
    if (mpz_odd_p(x.get_mpz_t())) {
        return (3 * x + 1) / 2;
    }
    return x / 2;
}

} // namespace

// The verifier's global max excursion must match the independent linear
// verifier over the same interval, exactly and at the same argument.
TEST_CASE("verify max excursion matches linear::verify_range") {
    vf::Options o;
    o.N = kN;
    o.A = 0; // pure Step-1 reference (the shipped default is A=10, so pin it).
    o.threads = 8;
    const vf::Result r = vf::run(o);

    const u128 hi = static_cast<u128>(1) << kN;
    collatz::linear::Options lo;
    lo.sieve_bits = 20;
    lo.mod9 = true;
    lo.threads = 8;
    const collatz::linear::Result lr = collatz::linear::verify_range(1, hi, lo);

    MESSAGE("N=" << kN << " candidates_checked=" << r.candidates_checked
                 << " max_excursion=" << collatz::to_string(r.max_excursion) << " @ "
                 << collatz::to_string(r.max_excursion_n));
    CHECK(r.candidates_checked > 0);
    CHECK(r.max_excursion == lr.max_excursion);
    CHECK(r.max_excursion_n == lr.max_excursion_n0);
}

// Every candidate handed to Step 3 must be undecided: its trajectory does not
// drop strictly below n0 within the first N accelerated steps (else Step 1
// would have excluded it). One survivor in 4096 is checked in GMP.
TEST_CASE("sampled step3 candidates do not descend within N steps (GMP)") {
    std::uint64_t seen = 0;
    std::uint64_t checked = 0;
    collatz::step1::Options opts;
    collatz::step1::enumerate_parallel(kN, 8, opts, [&](const collatz::step1::Node& node) {
        // Every 4096th survivor to arrive, over all threads.
        const std::uint64_t idx = __atomic_fetch_add(&seen, 1, __ATOMIC_RELAXED);
        if ((idx & 0xFFFu) != 0) {
            return;
        }
        const mpz_class n0 = collatz::oracle::to_mpz(node.n0);
        mpz_class x = n0;
        bool dropped = false;
        for (unsigned j = 1; j <= kN; ++j) {
            x = step_T(x);
            if (x < n0) {
                dropped = true;
                break;
            }
        }
        if (dropped) {
            REQUIRE_MESSAGE(false,
                            "survivor " << collatz::to_string(node.n0) << " descends within " << kN
                                        << " steps");
        }
        __atomic_fetch_add(&checked, 1, __ATOMIC_RELAXED);
    });
    MESSAGE("sampled " << checked << " of " << seen << " survivors");
    CHECK(checked > 0);
}
