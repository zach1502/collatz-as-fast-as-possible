#pragma once

// Helpers of test_u128_limbs/u128_limbs_06_lane_n86.cpp: the 16-step block walk
// in GMP (one T step at a time, no width limit), the slow lane (u128q_tail16_resume
// from the start) and one chain as a tail_persistent slot walks it without
// COLLATZ_GPU_TAIL_U64N (u128q_lane_fast_ok_n86, u128q_lane_cond_n86, the
// U128Q_U96_GE_N exit test and the resume with the rebuilt n).
#include "collatz/gpu/u128_limbs.h"
#include "collatz/u128.hpp"
#include "limb_reference.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gmp.h>
#include <vector>

namespace collatz::test::limbs {

[[nodiscard]] inline const std::array<std::uint32_t, 1u << 16>& lane_tbl16() {
    static const std::array<std::uint32_t, 1u << 16> t = build_tbl16_local();
    return t;
}

[[nodiscard]] inline const std::vector<std::uint32_t>& lane_split8() {
    static const std::vector<std::uint32_t> img = [] {
        std::vector<std::uint32_t> v(2u * u128q_split_entries());
        u128q_split_fill(v.data());
        return v;
    }();
    return img;
}

// The 16-step block walk in GMP, one T step at a time: the spec of
// u128q_tail16 with no width limit (overflow = a landing value >= 2^128).
class GmpWalk {
public:
    GmpWalk() {
        mpz_inits(n_, v_, w_, mx_, nullptr);
    }
    ~GmpWalk() {
        mpz_clears(n_, v_, w_, mx_, nullptr);
    }
    GmpWalk(const GmpWalk&) = delete;
    GmpWalk& operator=(const GmpWalk&) = delete;

    [[nodiscard]] TailOut run(u128 n, u128 v0) {
        set(n_, n);
        set(v_, v0);
        mpz_set(mx_, v_);
        TailOut out{0u, 0, 0u, 0};
        while (mpz_cmp(v_, n_) >= 0) {
            if (out.blocks >= U128Q_TAIL16_MAX_BLOCKS) {
                out.ovf = 1u;
                break;
            }
            if (mpz_cmp(v_, mx_) > 0) {
                mpz_set(mx_, v_);
            }
            mpz_set(w_, v_);
            for (int s = 0; s < 16; ++s) {
                if (mpz_odd_p(w_) != 0) {
                    mpz_mul_ui(w_, w_, 3u);
                    mpz_add_ui(w_, w_, 1u);
                }
                mpz_fdiv_q_2exp(w_, w_, 1u);
            }
            if (mpz_sizeinbase(w_, 2) > 128u) {
                out.ovf = 1u;
                break;
            }
            mpz_swap(v_, w_);
            ++out.blocks;
        }
        out.max_v = get(mx_);
        out.v_end = get(v_);
        return out;
    }

private:
    static void set(mpz_t z, u128 x) {
        const std::uint64_t w[2] = {collatz::lo64(x), collatz::hi64(x)};
        mpz_import(z, 2, -1, sizeof(std::uint64_t), 0, 0, w);
    }
    [[nodiscard]] static u128 get(const mpz_t z) {
        std::uint64_t w[2] = {0, 0};
        std::size_t count = 0;
        mpz_export(w, &count, -1, sizeof(std::uint64_t), 0, 0, z);
        return collatz::make_u128(w[1], w[0]);
    }
    mpz_t n_;
    mpz_t v_;
    mpz_t w_;
    mpz_t mx_;
};

// The slow lane: u128q_tail16_resume from the start (v == max, no block done).
[[nodiscard]] inline TailOut run_slow(u128 n, u128 v) {
    u128q mx;
    u128q ve;
    u128q_u32 ovf = 0;
    const u128q vq = from_native(v);
    const u128q_u32 b =
        u128q_tail16_resume(from_native(n), vq, vq, 0u, lane_tbl16().data(), &mx, &ovf, &ve);
    return {b, to_native(mx), ovf, to_native(ve)};
}

// One chain as a slot walks it without U64N. `stop` leaves the lane loop
// after that many blocks as well, so the resume runs from every lane state;
// `wrong_exit` uses the n < 2^64 exit rule instead (the non-vacuity check).
// *n_fold is the n that SLOT_RETIRE folds, *lane whether the lane loop ran.
[[nodiscard]] inline TailOut
run_lane(u128 n, u128 v, u128q_u32 stop, bool wrong_exit, u128* n_fold, bool* lane) {
    const u128q nq = from_native(n);
    const u128q vq = from_native(v);
    *n_fold = n;
    *lane = u128q_lane_fast_ok_n86(nq, vq) != 0;
    if (!*lane) {
        return run_slow(n, v);
    }
    const u128q_u64 n_lo = u128q_lo64(nq);
    const u128q_u32 n_hi = nq.w[2];
    u128q_u64 lo = u128q_lo64(vq);
    u128q_u32 hi = vq.w[2];
    u128q_u32 x = u128q_x54(lo, hi);
    u128q_u32 blocks = 0;
    while (blocks < stop && (wrong_exit ? u128q_lane_cond(lo, hi, n_lo, blocks)
                                        : u128q_lane_cond_n86(lo, hi, n_lo, n_hi, blocks))) {
        u128q_split_step_maxhi(lane_split8().data(), &lo, &hi, &x, &blocks);
    }
    u128q mx = u128q_from_x54(x);
    u128q ve = u128q_from_halves(lo, hi);
    const u128q nf = u128q_from_halves(n_lo, n_hi);
    u128q_u32 ovf = 0;
    const bool ge = wrong_exit ? (hi != 0u || lo >= n_lo) : U128Q_U96_GE_N(hi, lo, n_hi, n_lo);
    if (ge) {
        blocks = u128q_tail16_resume(nf, ve, mx, blocks, lane_tbl16().data(), &mx, &ovf, &ve);
    }
    *n_fold = to_native(nf);
    return {blocks, to_native(mx), ovf, to_native(ve)};
}

} // namespace collatz::test::limbs
