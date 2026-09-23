#include "collatz/oracle.hpp"
#include "collatz/threading.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "verify_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace collatz::verify {

using detail::kPreflightBits;

// Un-walked-trajectory peak bound. Numbers excluded by a Step-1 class
// certificate (within the first M steps) or a Step-2 window certificate (steps
// M..M+B) are never walked, so their trajectory peaks before the merge/descent
// step are never observed. Every such un-walked value is T^j(x), j <= K, of some
// x < 2^N, hence < (x+1)*(3/2)^K <= 2^N*(3/2)^K. If the reported record strictly
// exceeds that bound, no un-walked value can equal or beat it, so the reported
// max/argmax is the exact global one.
unsigned unwalked_depth(const Options& opts) noexcept {
    // M-step class-certificate prefix + B-step Step-2 window (when A > 0).
    const unsigned M = opts.A < opts.N ? opts.N - opts.A : opts.N;
    unsigned depth = M;
    if (opts.A > 0) {
        depth += opts.B;
    }
    return depth;
}

double unwalked_bound_log2(unsigned N, unsigned depth, u128 block_h) noexcept {
    // log2(2^N * (3/2)^depth) = N + depth*(log2 3 - 1).
    constexpr double kLog2ThreeHalves = 0.5849625007211562; // log2(3) - 1
    const double full = static_cast<double>(N) + static_cast<double>(depth) * kLog2ThreeHalves;
    const u128 h1 = block_h + 1; // block_h < 2^96 in any runnable block
    const double h1d =
        std::ldexp(static_cast<double>(hi64(h1)), 64) + static_cast<double>(lo64(h1));
    return full + std::log2(h1d);
}

unsigned value_bits(const Options& opts) noexcept {
    // (h+1)*2^N - 1 = h*2^N + (2^N - 1): its bit length is N + bit_length(h).
    return opts.N + static_cast<unsigned>(bit_length(opts.block_h));
}

u128 block_start(const Options& opts) noexcept {
    return opts.block_h << opts.N;
}

void validate_block(const Options& opts) {
    if (!opts.record_threshold.empty()) {
        std::string canon;
        if (!canonical_decimal(opts.record_threshold, canon) || canon != opts.record_threshold) {
            throw std::invalid_argument(
                "verify: record_threshold must be a decimal integer without leading zeros");
        }
        if (opts.records_cap == 0) {
            throw std::invalid_argument("verify: records_cap must be at least 1");
        }
    }
    if (opts.block_h == 0) {
        return;
    }
    // Step 1 starts at depth 2 (only n == 3 mod 4 survives the root), so
    // M = K - A < 2 would enumerate no class: every member n == 3 (mod 4) of the
    // block would be left with neither a walk nor a certificate. A >= N is
    // tested first, so N - A cannot wrap.
    if (opts.A == 0 || opts.A >= opts.N || opts.N - opts.A < 2u) {
        throw std::invalid_argument("verify: block mode needs 0 < A <= K - 2 (the survivor lift "
                                    "runs before Step 2, and Step 1 needs M = K - A >= 2)");
    }
    // The block end (h+1)*2^K must stay <= 2^96. Every lifted Step-1 value and
    // prefix bound (m' + 3^f a, pmax' + smax a, T^j(n) for j <= M) is below
    // 2 * (h+1)*2^K*(3/2)^M (Lemma A; pmax <= 2*3^M, smax <= 3^M), so
    // (h+1)*2^K*3^M < 2^(125+M) keeps them below 2^126 and the u128 lift exact.
    if (value_bits(opts) > 96) {
        throw std::invalid_argument("verify: block end (H+1)*2^K exceeds 2^96");
    }
    const unsigned M = opts.N - opts.A;
    mpz_class top = oracle::to_mpz(opts.block_h + 1);
    mpz_mul_2exp(top.get_mpz_t(), top.get_mpz_t(), opts.N);
    mpz_class lhs;
    mpz_ui_pow_ui(lhs.get_mpz_t(), 3, M);
    lhs *= top;
    mpz_class rhs = 1;
    mpz_mul_2exp(rhs.get_mpz_t(), rhs.get_mpz_t(), 125u + M);
    if (lhs >= rhs) {
        throw std::invalid_argument(
            "verify: block too large for the u128 lift ((H+1)*2^K*(3/2)^(K-A) >= 2^125); "
            "raise --A or lower H/K");
    }
    if (block_start(opts) < (static_cast<u128>(1) << kPreflightBits)) {
        throw std::invalid_argument(
            "verify: block start H*2^K must be >= 2^17 (no preflight runs in block mode)");
    }
}

std::string max_excursion_decimal(const Result& r) {
    return r.max_excursion_wide.empty() ? collatz::to_string(r.max_excursion)
                                        : r.max_excursion_wide;
}

unsigned max_threads() noexcept {
    // 64-bit product: no wrap for any hardware_threads() value.
    const std::uint64_t four_per_hw = 4u * static_cast<std::uint64_t>(hardware_threads());
    return static_cast<unsigned>(
        std::min<std::uint64_t>(std::max<std::uint64_t>(256u, four_per_hw), 0xFFFFFFFFu));
}

double max_excursion_log2(const Result& r) {
    const mpz_class v(max_excursion_decimal(r), 10);
    if (v == 0) {
        return 0.0;
    }
    long exp = 0;
    const double d = mpz_get_d_2exp(&exp, v.get_mpz_t()); // v = d * 2^exp, d in [0.5, 1)
    return std::log2(d) + static_cast<double>(exp);
}

bool result_max_exact(unsigned N, u128 block_h, unsigned depth, const Result& r) {
    if (block_h == 0 && N <= kPreflightBits) {
        return true; // the preflight walks every n < 2^N directly
    }
    if (r.max_excursion_wide.empty() && (r.max_exceeds_u128 || r.max_excursion == 0)) {
        return false; // no record, or a saturated one whose exact value is unknown
    }
    // (h+1) * 2^N * 3^depth < 2^depth * R, in GMP (3^depth overflows u128).
    mpz_class lhs;
    mpz_ui_pow_ui(lhs.get_mpz_t(), 3, depth);
    lhs *= oracle::to_mpz(block_h + 1);
    mpz_mul_2exp(lhs.get_mpz_t(), lhs.get_mpz_t(), N);
    mpz_class rhs(max_excursion_decimal(r), 10);
    mpz_mul_2exp(rhs.get_mpz_t(), rhs.get_mpz_t(), depth);
    return mpz_cmp(lhs.get_mpz_t(), rhs.get_mpz_t()) < 0;
}

namespace {

/// Sign of L - 2^71 for the block start L = block_h * 2^N (exact).
[[nodiscard]] int block_start_vs_2_71(const Options& opts) {
    mpz_class L = oracle::to_mpz(opts.block_h);
    mpz_mul_2exp(L.get_mpz_t(), L.get_mpz_t(), opts.N);
    mpz_class p71 = 1;
    mpz_mul_2exp(p71.get_mpz_t(), p71.get_mpz_t(), 71);
    return mpz_cmp(L.get_mpz_t(), p71.get_mpz_t());
}

} // namespace

std::string assumption_source(const Options& opts) {
    if (!opts.assumption_source.empty()) {
        return opts.assumption_source;
    }
    if (opts.block_h == 0) {
        return kAssumptionNone;
    }
    return block_start_vs_2_71(opts) <= 0 ? kAssumptionBarina71 : kAssumptionPriorBlocks;
}

std::string assumes_below(const Options& opts) {
    mpz_class L = oracle::to_mpz(opts.block_h);
    mpz_mul_2exp(L.get_mpz_t(), L.get_mpz_t(), opts.N);
    return oracle::to_decimal(L);
}

std::string default_record_threshold(const Options& opts) {
    if (opts.block_h == 0 || block_start_vs_2_71(opts) < 0) {
        return {};
    }
    return kBarinaRecord97PeakT;
}

} // namespace collatz::verify
