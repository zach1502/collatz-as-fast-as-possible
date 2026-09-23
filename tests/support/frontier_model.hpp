#ifndef COLLATZ_TESTS_FRONTIER_MODEL_HPP
#define COLLATZ_TESTS_FRONTIER_MODEL_HPP

// Independent model of a frontier block run, shared by
// tests/integration/test_frontier_blocks*.cpp:
//   * block_model: ref/ Step-1 survivors (independent code, GMP), lifted by walking
//     n0 + L directly, the Step-2 decisions (mod 9, window dip from
//     ref::dip_B, per-extension partial OEE) evaluated on the concrete block
//     member, and a single-stepping Step-3 walk with a GMP fallback;
//   * brute_force: the exact max of Q over a range of numbers;
//   * pipeline_candidates: the shipped pipeline's candidate set rebuilt from
//     the production Step 1, lift and Filter::mask, and replay_exclusion, an
//     independent GMP check of one exclusion on the concrete number.
//
// frontier_model/walk.hpp holds the run options and the walks of one number,
// frontier_model/block_model.hpp the block model, the brute force and the
// checks against them, and frontier_model/shipped.hpp the shipped candidate set
// and the GMP replay of its exclusions.

#include "frontier_model/block_model.hpp"
#include "frontier_model/shipped.hpp"
#include "frontier_model/walk.hpp"

#endif // COLLATZ_TESTS_FRONTIER_MODEL_HPP
