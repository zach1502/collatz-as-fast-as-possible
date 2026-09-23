#ifndef COLLATZ_POW3_HPP
#define COLLATZ_POW3_HPP

// Compile-time table of powers of three, 3^0 .. 3^80.
//
// 3^80 is the largest power of three below 2^127 (3^80 has 127 bits, 3^81 has
// 129 bits and would overflow u128). The table and the constexpr accessor let
// hot paths avoid repeated multiplication.

#include "collatz/u128.hpp"

#include <array>
#include <cstddef>

namespace collatz {

/// Highest exponent e such that 3^e fits below 2^127 (inclusive table bound).
inline constexpr unsigned pow3_max_exp = 80;

/// Number of entries in POW3: exponents 0 .. pow3_max_exp.
inline constexpr std::size_t pow3_count = pow3_max_exp + 1;

namespace detail {

[[nodiscard]] constexpr std::array<u128, pow3_count> make_pow3_table() noexcept {
    std::array<u128, pow3_count> table{};
    u128 value = 1;
    for (std::size_t i = 0; i < pow3_count; ++i) {
        table[i] = value;
        value *= 3;
    }
    return table;
}

} // namespace detail

/// POW3[e] == 3^e for e in [0, 80]. All entries fit in u128.
inline constexpr std::array<u128, pow3_count> POW3 = detail::make_pow3_table();

static_assert(bit_length(POW3[pow3_max_exp]) < 128, "3^80 must fit in u128");
static_assert(bit_length(POW3[pow3_max_exp]) == 127, "3^80 is expected to be a 127-bit number");

/// 3^e for e in [0, 80]. e must be at most 80, since 3^81 does not fit in u128
/// and is not in the table.
[[nodiscard]] constexpr u128 pow3(unsigned e) noexcept {
    return POW3[e];
}

} // namespace collatz

#endif // COLLATZ_POW3_HPP
