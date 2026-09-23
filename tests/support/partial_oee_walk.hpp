#pragma once

// The partial-OEE threshold a* of a Step-1 class by a walk of its M-step prefix,
// the reference for the a* that the Step-1 DFS carries (step1::Node::oee_astar).
#include "collatz/collatz_fn.hpp"
#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace collatz::test {

/// The smallest a in [0, 2^A) from which every extension n = n0 + a*2^M is
/// excluded by an OEE arm of the class, or UINT32_MAX when no a is. An arm is a
/// depth j < M with m_j = T^j(n0) odd whose odd run is followed by two even
/// steps, all within the first M steps. With f_j odd steps before j, the arm
/// excludes every a with a*C > D, where C = 2^{M-j}(2^{j+1} - 3^{f_j}) > 0 and
/// D = m_j - 1 - 2 n0: a* = D/C + 1 for D >= 0 (at D == 0 the merge target is
/// n0 itself, so a = 0 stays) and 0 for D < 0. Requires M <= 76, so every
/// T^j(n0) < 3^M < 2^128.
inline std::uint32_t partial_oee_walk(u128 n0, unsigned M, unsigned A) {
    assert(M <= 76 && A <= 14);
    std::vector<u128> mval(M + 1);
    std::vector<unsigned> fval(M + 1, 0);
    std::vector<bool> odd(M);
    mval[0] = n0;
    for (unsigned j = 0; j < M; ++j) {
        odd[j] = (mval[j] & 1u) != 0u;
        fval[j + 1] = fval[j] + (odd[j] ? 1u : 0u);
        mval[j + 1] = T(mval[j]).value;
    }
    // first_even[j]: the first even step at or after j (M if none).
    std::vector<unsigned> first_even(M + 1, M);
    for (unsigned j = M; j-- > 0;) {
        first_even[j] = odd[j] ? first_even[j + 1] : j;
    }
    const u128 two_a = u128{1} << A;
    u128 best = two_a;
    for (unsigned j = 0; j < M; ++j) {
        const unsigned e = first_even[j];
        if (!odd[j] || e + 1u >= M || odd[e + 1u]) {
            continue;
        }
        const u128 two_j1 = u128{1} << (j + 1u);
        const u128 p3 = pow3(fval[j]);
        if (two_j1 <= p3) {
            continue; // the slope is not positive: the arm excludes nothing.
        }
        const u128 c = (u128{1} << (M - j)) * (two_j1 - p3);
        const u128 lhs = mval[j] - 1u;
        const u128 rhs = 2u * n0;
        best = std::min(best, lhs >= rhs ? (lhs - rhs) / c + 1u : u128{0});
    }
    return best >= two_a ? std::numeric_limits<std::uint32_t>::max()
                         : static_cast<std::uint32_t>(best);
}

} // namespace collatz::test
