#ifndef COLLATZ_STEP3_HPP
#define COLLATZ_STEP3_HPP

// "Step 3" trajectory finisher for the Collatz verifier.
//
// After Steps 1 and 2 have advanced a candidate n through k accelerated steps to
// m = T^k(n) >= n, Step 3 continues the trajectory until it first falls
// strictly below n. That strict drop is the descent certificate the induction
// relies on.
//
// collatz/step3/exact.hpp declares the GMP finisher that counts exact T-steps
// and collatz/step3/block.hpp the block-granular finishers and their batch
// drivers.

#include "collatz/step3/block.hpp"
#include "collatz/step3/exact.hpp"

#endif // COLLATZ_STEP3_HPP
