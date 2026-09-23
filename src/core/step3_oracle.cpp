// Step 3 GMP replays of one candidate: finish_oracle (T-steps) and
// finish_block16_oracle (16-step blocks), and the exact excursion
// first_drop_peak, which walks in u128 and replays in GMP.

#include "collatz/collatz_fn.hpp"
#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/step3.hpp"
#include "collatz/u128.hpp"
#include "step3_internal.hpp"

#include <cstdint>
#include <gmp.h>
#include <gmpxx.h>
#include <string>

namespace collatz::step3 {

using namespace detail;

Outcome finish_oracle(u128 n, u128 m, std::string* max_dec) {
    Outcome o;
    o.max_seen = m;
    if (max_dec != nullptr) {
        max_dec->clear();
    }
    if (n <= 1) {
        return o;
    }

    const mpz_class start = oracle::to_mpz(n);
    mpz_class v = oracle::to_mpz(m);
    mpz_class maxseen = v;
    while (v >= start) {
        if (mpz_even_p(v.get_mpz_t())) {
            v /= 2;
        } else {
            v = (3 * v + 1) / 2;
        }
        if (v > maxseen) {
            maxseen = v;
        }
    }
    if (mpz_sizeinbase(maxseen.get_mpz_t(), 2) > 128) {
        o.max_exceeds_u128 = true;
        o.max_seen = u128_max;
        if (max_dec != nullptr) {
            *max_dec = oracle::to_decimal(maxseen);
        }
    } else {
        o.max_seen = mpz_to_u128_saturating(maxseen);
    }
    return o;
}

Outcome first_drop_peak(u128 n, std::string* max_dec, unsigned max_steps) {
    Outcome o;
    o.max_seen = n;
    if (max_dec != nullptr) {
        max_dec->clear();
    }
    if (n <= 1) {
        return o;
    }
    u128 v = n;
    u128 mx = n;
    for (unsigned j = 0; j < max_steps; ++j) {
        const StepResult sr = collatz::T(v);
        if (sr.overflow) {
            return finish_oracle(n, n, max_dec);
        }
        v = sr.value;
        if (v < n) {
            break;
        }
        if (v > mx) {
            mx = v;
        }
    }
    o.max_seen = mx;
    return o;
}

BlockOutcome finish_block16_oracle(u128 n, u128 m, std::string* max_dec) {
    BlockOutcome o;
    o.max_seen = m;
    if (max_dec != nullptr) {
        max_dec->clear();
    }
    // Trivial cycle {1,2} is the induction base (see finish_block16): n == 2 is a
    // fixed point of the 16-step block map, so the block loop would spin on it.
    if (n <= 2) {
        o.v_end = n; // base case: the landing value is n itself (blocks == 0).
        return o;
    }

    const BlockTable16& tb = block_table16();
    const mpz_class start = oracle::to_mpz(n);
    mpz_class v = oracle::to_mpz(m);
    mpz_class maxseen = v;
    std::uint64_t blocks = 0;
    mpz_class high;
    mpz_class w;

    while (v >= start) {
        // Exact peak of this block by single-stepping in GMP (never overflows).
        w = v;
        for (unsigned i = 0; i < kBlockStep; ++i) {
            if (mpz_even_p(w.get_mpz_t())) {
                w /= 2;
            } else {
                w = (3 * w + 1) / 2;
            }
            if (w > maxseen) {
                maxseen = w;
            }
        }
        // One block with the same packed-table landing arithmetic; 3^16 = 43046721
        // fits a long, so the multiply is exact in GMP.
        const auto r = static_cast<std::uint32_t>(mpz_fdiv_ui(v.get_mpz_t(), 1u << kBlockStep));
        const std::uint32_t packed = tb[r];
        const unsigned f = packed & kFMask;
        const auto c = static_cast<unsigned long>(packed >> 5u);
        high = v >> kBlockStep;
        v = high * mpz_class(static_cast<unsigned long>(collatz::lo64(pow3(f)))) + c;
        ++blocks;
    }

    o.blocks = blocks;
    if (mpz_sizeinbase(maxseen.get_mpz_t(), 2) > 128) {
        o.max_seen = u128_max;
        if (max_dec != nullptr) {
            *max_dec = oracle::to_decimal(maxseen);
        }
    } else {
        o.max_seen = mpz_to_u128_saturating(maxseen);
    }
    o.v_end = mpz_to_u128_saturating(v); // v < n < 2^64 at exit, fits u128.
    return o;
}

} // namespace collatz::step3
