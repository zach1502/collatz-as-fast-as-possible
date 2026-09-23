#ifndef COLLATZ_TESTS_REF_RANDOM_AUDIT_HPP
#define COLLATZ_TESTS_REF_RANDOM_AUDIT_HPP

// Randomized differential comparison of the production pipeline (verify::run)
// with the independent reference (ref::verify_ref, ref/) over seeded random
// (N, A, B, t_w, D, mod3t, num_bitvectors, partial OEE, threads) and over every
// bitvector-cache state:
//
//   cold        bv_cache = false (the --no-bv-cache path; nothing on disk);
//   cold-write  bv_cache = true into a fresh private COLLATZ_CACHE_DIR (a miss:
//               builds, then writes the dip + packed-bitvector files);
//   warm        the same private dir again (must be a hit: bv_cache_hit);
//   shared      one private dir reused by every config of the sweep, so entries
//               written by other (B, M, nbv, t_w, D) tuples sit next to this
//               one -- a cache key that omits a parameter loads the wrong
//               bitvectors here and diverges from ref/.
//
// Compared, field for field: candidates_checked, coverage_checksum,
// pow_checksum (pow result digest, table16), blocks_total, max_excursion,
// max_excursion_n, max_exceeds_u128, and max_exact. ref/ has no max_exact, so
// it is recomputed here independently from its definition in
// collatz/verify/bounds.hpp (exact iff 2^N * 3^K < 2^K * record with
// K = (N - A) + B, GMP) using the ref/ record, not the production one.
//
// Scope / gaps (what ref/ does not model, so the draw never enables it):
// only the table16 kernel (pow is kernel-specific), no case splitting and no
// GPU. Both sides run all three Step-1 sieves (descent, path merge, OEE).
// ref::dip_B requires B <= 32 and ref::survivors M = N - A <= 26; the draw
// stays far inside both for speed.
//
// A failing config prints its sweep seed and index;
// COLLATZ_AUDIT_SEED=<seed> COLLATZ_AUDIT_ONLY=<index> reruns it.
//
// ref_random_audit/draw.hpp holds the draw of a configuration and
// ref_random_audit/sweep.hpp the cache-mode runs, the comparison and the sweep.

#include "ref_random_audit/draw.hpp"
#include "ref_random_audit/sweep.hpp"

#endif // COLLATZ_TESTS_REF_RANDOM_AUDIT_HPP
