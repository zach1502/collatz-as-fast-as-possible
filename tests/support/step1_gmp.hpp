#pragma once

// GMP walk helpers, the exclusion record and the sorted Step-1 survivor list
// shared by the unit and integration tests.
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

#include <algorithm>
#include <gmpxx.h>
#include <vector>

namespace collatz::test {

inline mpz_class step_T(const mpz_class& x) {
    if (mpz_odd_p(x.get_mpz_t())) {
        return (3 * x + 1) / 2;
    }
    return x / 2;
}

inline mpz_class iterate_T(mpz_class x, unsigned k) {
    for (unsigned i = 0; i < k; ++i) {
        x = step_T(x);
    }
    return x;
}

inline bool is_odd(const mpz_class& x) {
    return mpz_odd_p(x.get_mpz_t()) != 0;
}

struct Excl {
    u128 n0;
    unsigned depth;
    int reason;
};

/// Every survivor of step1(M), sorted ascending by n0, from the serial enumerate.
inline std::vector<step1::Node> survivors(unsigned M, const step1::Options& opts = {}) {
    std::vector<step1::Node> out;
    step1::enumerate(M, opts, [&out](const step1::Node& n) { out.push_back(n); });
    std::sort(out.begin(), out.end(), [](const step1::Node& a, const step1::Node& b) {
        return a.n0 < b.n0;
    });
    return out;
}

} // namespace collatz::test
