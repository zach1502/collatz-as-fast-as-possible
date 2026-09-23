#pragma once

// Step-1 survivor sampling for the Step-2 unit tests (test_step2.cpp).
#include "collatz/step1.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace collatz::test {

// Reservoir-sample up to `k` Step-1 survivors of step1(M) so the samples cover
// the whole class range without materializing every survivor.
inline std::vector<step1::Node> sample_survivors(unsigned M, std::size_t k, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<step1::Node> sample;
    sample.reserve(k);
    std::uint64_t seen = 0;
    collatz::step1::enumerate(M, collatz::step1::Options{}, [&](const step1::Node& s) {
        ++seen;
        if (sample.size() < k) {
            sample.push_back(s);
        } else {
            const std::uint64_t r = rng() % seen;
            if (r < k) {
                sample[static_cast<std::size_t>(r)] = s;
            }
        }
    });
    return sample;
}

} // namespace collatz::test
