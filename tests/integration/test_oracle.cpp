#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/collatz_fn.hpp"
#include "collatz/oracle.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <random>

using collatz::u128;

namespace {

// Seed count: 10^6 in optimized builds and 20,000 under sanitizers, so that a
// sanitizer run stays short.
const int kSeeds = static_cast<int>(collatz::test::scaled(1'000'000, 20'000));

constexpr std::uint64_t kMaxK = 64;

// Exact u128 -> mpz by importing the two 64-bit limbs into a caller-owned mpz
// (no allocation, no decimal round-trip), for the per-step comparison. The
// decimal-string collatz::oracle::to_mpz still runs on every seed's start value
// and final native value below, so both conversions are covered without a
// to_string + mpz_set_str on every step of the 64-step inner loop.
void set_mpz_u128(mpz_class& out, u128 x) {
    const std::uint64_t limbs[2] = {collatz::lo64(x), collatz::hi64(x)};
    mpz_import(out.get_mpz_t(),
               2,
               /*order=*/-1,
               sizeof(std::uint64_t),
               /*endian=*/0,
               /*nails=*/0,
               limbs);
}

} // namespace

TEST_CASE("differential: native T lockstep-agrees with GMP oracle for k=1..64") {
    std::mt19937_64 rng(0xC011A72ULL);
    // Mask to [1, 2^127): clear the top bit, force nonzero.
    constexpr u128 mask127 = (static_cast<u128>(1) << 127) - 1;

    long long compared = 0;
    for (int i = 0; i < kSeeds; ++i) {
        const std::uint64_t hi = rng();
        const std::uint64_t lo = rng();
        u128 n = collatz::make_u128(hi, lo) & mask127;
        if (n == 0) {
            n = 1;
        }

        // Native and GMP walk in lockstep, one T-step at a time, compared after
        // each of the first kMaxK steps. The walk stops early if the native step
        // overflows u128 (the arbitrary-precision oracle would then diverge upward).
        u128 native = n;
        mpz_class ref = collatz::oracle::to_mpz(n);
        mpz_class native_mpz;
        for (std::uint64_t k = 0; k < kMaxK; ++k) {
            const auto step = collatz::T(native);
            if (step.overflow) {
                break;
            }
            native = step.value;
            if (mpz_odd_p(ref.get_mpz_t())) {
                ref = (3 * ref + 1) / 2;
            } else {
                ref /= 2;
            }
            set_mpz_u128(native_mpz, native);
            if (native_mpz != ref) {
                // The first mismatch reports its seed and ends the test.
                REQUIRE_MESSAGE(false, "mismatch at seed index " << i << " step " << (k + 1));
            }
            ++compared;
        }
        // The decimal-string conversion must agree with the walk's end value too.
        if (collatz::oracle::to_mpz(native) != ref) {
            REQUIRE_MESSAGE(false, "to_mpz mismatch at seed index " << i);
        }
    }
    CHECK(compared > 0);
    MESSAGE("compared " << compared << " (seed,step) pairs across " << kSeeds << " seeds");
}
