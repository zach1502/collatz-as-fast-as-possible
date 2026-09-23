#pragma once

// Block-mode helpers shared by the frontier, verify and checkpoint unit tests.
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

namespace collatz::test {

inline collatz::u128 pow2(unsigned k) {
    return static_cast<collatz::u128>(1) << k;
}

inline collatz::verify::Options
block_opts(unsigned K, collatz::u128 H, unsigned A = 10, unsigned B = 16) {
    collatz::verify::Options o;
    o.N = K;
    o.block_h = H;
    o.A = A;
    o.B = B;
    o.partial_oee = true;
    o.step1.mod3t = 6;
    o.bv_cache = false;
    return o;
}

} // namespace collatz::test
