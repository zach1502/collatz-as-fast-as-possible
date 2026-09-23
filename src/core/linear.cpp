// The u128 walk with its GMP fallback, the per-value sieve tests and
// verify_range. linear_sieve.cpp builds the 2^k descent sieve.

#include "collatz/linear.hpp"

#include "collatz/oracle.hpp"
#include "collatz/pow3.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gmp.h>
#include <gmpxx.h>
#include <thread>
#include <vector>

namespace collatz::linear {

namespace {

/// Converts an exact mpz max excursion to u128, saturating to u128_max when it
/// does not fit (only on the u128 overflow path).
[[nodiscard]] u128 mpz_to_u128_saturating(const mpz_class& m) {
    if (mpz_sizeinbase(m.get_mpz_t(), 2) > 128) {
        return u128_max;
    }
    u128 out = 0;
    if (from_string(oracle::to_decimal(m), out)) {
        return out;
    }
    return u128_max;
}

using Clock = std::chrono::steady_clock;

/// Per-thread accumulator; combined deterministically after the join.
struct Partial {
    std::uint64_t checksum = 0;
    u128 maxv = 0;
    u128 maxn0 = 0;
    std::uint64_t processed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t overflow = 0;
};

} // namespace

bool Sieve2k::skip(u128 n) const noexcept {
    if (k_ == 0) {
        return false;
    }
    const u128 mask = (static_cast<u128>(1) << k_) - 1;
    const std::uint64_t r = static_cast<std::uint64_t>(n & mask);
    return ((bitmap_[static_cast<std::size_t>(r >> 6)] >> (r & 63u)) & 1u) != 0u;
}

bool mod9_skip(u128 n) noexcept {
    const unsigned r = static_cast<unsigned>(n % 9);
    return r == 2u || r == 4u || r == 5u || r == 8u;
}

std::uint64_t verify_one_oracle(u128 n0, mpz_class& max_seen) {
    const mpz_class start = oracle::to_mpz(n0);
    max_seen = start;
    if (n0 <= 1) {
        return 0; // n0 == 1 is the fixed point; n0 == 0 is out of domain
    }
    mpz_class n = start;
    mpz_class p3;
    std::uint64_t checksum = 0;
    while (n >= start) {
        n += 1;
        // alpha = ctz(n); n = (n >> alpha) * 3^alpha
        const mp_bitcnt_t a = mpz_scan1(n.get_mpz_t(), 0);
        mpz_tdiv_q_2exp(n.get_mpz_t(), n.get_mpz_t(), a);
        mpz_ui_pow_ui(p3.get_mpz_t(), 3, a);
        n *= p3;
        checksum += static_cast<std::uint64_t>(a);
        n -= 1; // the run's even peak 3^a * q - 1
        if (n > max_seen) {
            max_seen = n;
        }
        // beta = ctz(n); n >>= beta: the even steps down to the next odd value
        const mp_bitcnt_t b = mpz_scan1(n.get_mpz_t(), 0);
        mpz_tdiv_q_2exp(n.get_mpz_t(), n.get_mpz_t(), b);
    }
    return checksum;
}

OneResult verify_one_detailed(u128 n0) {
    OneResult res;
    res.max_seen = n0;
    if (n0 <= 1) {
        return res; // 1 is the fixed point, where the loop below would not end
    }

    // u128 path. After the first iteration n is odd, and each iteration computes
    //   n += 1; a = ctz(n); n = (n >> a) * 3^a; n -= 1; b = ctz(n); n >>= b
    // The value after "n -= 1" is 3^a*q - 1, the peak of the odd run just taken
    // (each odd T-step multiplies by about 3/2), so the max over these values is
    // the exact excursion of the T map.
    u128 n = n0;
    u128 maxv = n0;
    std::uint64_t checksum = 0;
    bool overflowed = false;

    while (n >= n0) {
        bool ovf = false;
        const u128 np1 = checked_add(n, static_cast<u128>(1), ovf);
        if (ovf) {
            overflowed = true;
            break;
        }
        const unsigned a = static_cast<unsigned>(ctz(np1));
        if (a > pow3_max_exp) {
            overflowed = true; // 3^a would exceed u128 regardless of q
            break;
        }
        const u128 q = np1 >> a;
        const u128 prod = checked_mul(q, pow3(a), ovf);
        if (ovf) {
            overflowed = true;
            break;
        }
        checksum += a;
        const u128 peak = prod - 1;
        if (peak > maxv) {
            maxv = peak;
        }
        const unsigned b = static_cast<unsigned>(ctz(peak));
        n = peak >> b;
    }

    if (!overflowed) {
        res.checksum = checksum;
        res.max_seen = maxv;
        res.overflowed = false;
        return res;
    }

    // On overflow the partial result is discarded and n0 is walked again in GMP.
    mpz_class m;
    res.checksum = verify_one_oracle(n0, m);
    res.max_seen = mpz_to_u128_saturating(m);
    res.overflowed = true;
    return res;
}

Result verify_range(u128 lo, u128 hi, const Options& opts) {
    Result res;
    if (hi <= lo) {
        return res;
    }

    // The sieve is built before the timer starts, so `seconds` covers only the
    // verification, as in Barina's program, whose sieve is generated separately.
    Sieve2k sieve;
    const bool use_2k = opts.sieve_bits > 0;
    if (use_2k) {
        sieve.build(opts.sieve_bits);
    }
    const bool use_mod9 = opts.mod9;

    const u128 total = hi - lo;
    constexpr u128 kChunk = static_cast<u128>(1) << 20;
    const u128 total_chunks = (total + kChunk - 1) / kChunk;

    unsigned nthreads = opts.threads == 0 ? hardware_threads() : opts.threads;
    if (nthreads < 1) {
        nthreads = 1;
    }

    std::atomic<std::uint64_t> next_chunk{0};
    std::vector<Partial> partials(nthreads);

    const auto worker = [&](unsigned tid) {
        Partial& p = partials[tid];
        for (;;) {
            const std::uint64_t ci = next_chunk.fetch_add(1, std::memory_order_relaxed);
            if (static_cast<u128>(ci) >= total_chunks) {
                break;
            }
            const u128 c_lo = lo + static_cast<u128>(ci) * kChunk;
            u128 c_hi = c_lo + kChunk;
            if (c_hi > hi) {
                c_hi = hi;
            }
            for (u128 n0 = c_lo; n0 < c_hi; ++n0) {
                if (use_2k && sieve.skip(n0)) {
                    ++p.skipped;
                    continue;
                }
                if (use_mod9 && mod9_skip(n0)) {
                    ++p.skipped;
                    continue;
                }
                const OneResult r = verify_one_detailed(n0);
                p.checksum += r.checksum;
                if (r.overflowed) {
                    ++p.overflow;
                }
                ++p.processed;
                if (r.max_seen > p.maxv) {
                    p.maxv = r.max_seen;
                    p.maxn0 = n0;
                }
            }
        }
    };

    const auto t0 = Clock::now();
    if (nthreads == 1) {
        worker(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(nthreads);
        for (unsigned i = 0; i < nthreads; ++i) {
            pool.emplace_back(worker, i);
        }
        for (std::thread& t : pool) {
            t.join();
        }
    }
    const auto t1 = Clock::now();

    // The checksum is a wrapping sum, so its order does not matter, and a tie on
    // the max keeps the smallest n0, so the result does not depend on the thread
    // count.
    for (const Partial& p : partials) {
        res.checksum += p.checksum;
        res.numbers_processed += p.processed;
        res.numbers_skipped_by_sieve += p.skipped;
        res.overflow_fallbacks += p.overflow;
        if (p.processed == 0) {
            continue;
        }
        if (p.maxv > res.max_excursion ||
            (p.maxv == res.max_excursion && p.maxn0 < res.max_excursion_n0)) {
            res.max_excursion = p.maxv;
            res.max_excursion_n0 = p.maxn0;
        }
    }

    res.seconds = std::chrono::duration<double>(t1 - t0).count();
    return res;
}

} // namespace collatz::linear
