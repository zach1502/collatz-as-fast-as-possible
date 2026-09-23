#ifndef COLLATZ_STEP2_HPP
#define COLLATZ_STEP2_HPP

// Angeltveit-style "Step 2" lookahead bitvector sieve for the Collatz verifier.
//
// Step 1 (collatz/step1.hpp) sieves residue classes mod 2^M using only the low
// M bits, producing a set of survivor classes (n0, m = T^M(n0), f = odd-step
// count). Step 2 extends each survivor by A high bits: it decides, for the
// 2^A candidates n = n0 + a*2^{N-A} (a in [0, 2^A)), which ones can be pruned
// by a B-step lookahead past depth M, without touching the individual value.
//
// The method is that of arXiv:2602.10466, section 4.
//
// The dip function. The accelerated map is T(n) = n/2 (even) or (3n+1)/2
// (odd). Along the first k steps let f_k = #odd steps; then
// T^k(n) = (3^{f_k} n + c)/2^k, so T^k(n) < n (contraction) is governed by
// whether 2^k > 3^{f_k}. Using the rational bound 485/306 > log2(3)
// (equivalently 3^306 < 2^485; 306*log2(3) is just below 485), a contraction
// "margin" at step k is measured by the integer
//
//     A_k(m) = 306 k - 485 f_k(m)          (>= 0 means "has contracted").
//
// For a residue m in [0, 2^B), the B-step parity sequence (which depends only
// on m mod 2^B) gives a set S_B(m) of such margins from three sources, and
// dip_B(m) = max S_B(m):
//
//   (A) descent   : A_k(m) = 306k - 485 f_k(m),                    0 <= k <= B.
//   (B) path-merge: A_k(m) + 179 = 306(k-1) - 485(f_k(m)-1), when T^k(m) is
//                   the T-image of the strictly smaller (2 T^k(m) - 1)/3, i.e.
//                   T^k(m) == 2 (mod 3) with at least one odd step (f_k >= 1).
//                   For a candidate n, m_full = T^{N-A}(n) is known only mod
//                   2^B, and "T^k(m_full) == 2 (mod 3)" is not a function of
//                   m mod 2^B, so path-merge uses the parity-only criterion of
//                   Lemma 2.8 (an even number of even steps have elapsed since
//                   the last odd step). It agrees with the mod-3 test for the
//                   actual value m and depends only on the parity sequence
//                   (parity_mod3_is_2).
//   (C) OEE       : A_k(m) + 306 = 306(k+1) - 485 f_k(m), when T^k(m) begins
//                   l >= 1 odd steps followed by 2 even steps, all inside the
//                   B-step window (Hercher: (T^k(m)-1)/2 is a smaller merge
//                   target reached with one extra halving). Parity-only.
//
// dip_B(m) >= 0 always (k = 0 gives A_0 = 0).
//
// Theorem 4.4 (why bitvectors work). Given a Step-1 survivor (n0, m, f) and
// a in [0, 2^A), the candidate n = n0 + a*2^{N-A} has m_a = m + 3^f a and the
// same f. If
//
//     dip_B(m_a mod 2^B) >= 485 f - 306 (N-A)  =: threshold(f)
//
// then n descends below itself, or merges with a smaller number, within
// N-A+B steps, provided every iterate stays >= 99781. That proviso holds for
// the verifier because any iterate < 99781 <= n is already a descent for
// n >= 99781, and the preflight walks every n < 2^17, so candidates below 2^17
// are not decided here.
//
// So a candidate is pruned iff dip_B(m_a mod 2^B) >= threshold(f).
// Equivalently it must still be checked iff dip_B(m_a mod 2^B) < threshold(f).
//
// Bitvectors. threshold(f) grows with f, so there is one bitvector per relevant
// f. Let fmin = ceil((306(N-A)+1)/485) be the smallest f with a positive
// threshold. For i in [0, num_bitvectors): f = fmin + i, t_i = 485 f - 306(N-A),
// and BV_i is 2^B bits with
//
//     BV_i[j] = 1  iff  dip_B(j * 3^f mod 2^B) < t_i     (1 = "must check").
//
// Bit index j maps to residue j*3^f mod 2^B, so consecutive j step through
// m, m+3^f, m+2*3^f, ... -- exactly the m_a sequence. For a survivor with f:
//   * f <  fmin              -> 485f <= 306(N-A), so every extension is pruned
//                               (dip >= 0 >= t): mask = 0.
//   * i = f - fmin >= NBV    -> no lookahead available: mask = all ones.
//   * otherwise              -> j0 = m * inv(3^f) mod 2^B, and candidate a maps
//                               to bit j0 + a (mod 2^B); read the contiguous
//                               (wrapping) 2^A-bit window.
//
// A second, independent mod-9 preimage sieve removes n == 2,4,5,8 (mod 9). The
// final mask is (BV window) & (mod-9 mask).
//
// Wide A. The candidate mask is a fixed-capacity bit array (CandidateMask) of
// up to 256 uint64 words, so A up to 14 is supported (2^14 = 16384 bits = 256
// words = 2 KB). A <= 6 stays a one-word fast path (2^6 = 64 bits fit a single
// word) and A <= 10 a 16-word path. The mod-9 masks, the partial-OEE a* prefix
// mask, and the BV window read are all generalized to `words` words; the window
// read wraps the 2^B boundary word-by-word. Raising A amortizes every
// per-survivor fixed cost (the mod-9 and bitvector reads) over 4-256x more
// candidates while shrinking the Step-1 survivor set (larger M = N - A).
// The paper uses A=6 on the CPU and A=10 on the GPU, both with B=24.
//
// Overflow / precision: dip runs on residues m < 2^B <= 2^32, and T^k(m) stays
// below 3^B <= 3^32 < 2^51, so 64-bit arithmetic is exact throughout the build.
//
// collatz/step2/params.hpp declares the configuration and limits,
// collatz/step2/candidate_mask.hpp the candidate bitmask, and
// collatz/step2/filter.hpp the Filter class.

#include "collatz/step2/candidate_mask.hpp"
#include "collatz/step2/filter.hpp"
#include "collatz/step2/params.hpp"

#endif // COLLATZ_STEP2_HPP
