#ifndef COLLATZ_U128_HPP
#define COLLATZ_U128_HPP

// 128-bit unsigned arithmetic primitives for the Collatz verifier.
//
// Uses the compiler-native `unsigned __int128` of clang and GCC. MSVC has no
// `__int128` and is unsupported (README, section Build). The checked operations
// use the `__builtin_{add,mul}_overflow` intrinsics

#include <cstdint>
#include <string>
#include <string_view>

namespace collatz {

/// Native 128-bit unsigned integer. clang/GCC only; MSVC unsupported.
///
/// `__int128` is a compiler extension, so a strict `-Wpedantic -Werror` build
/// rejects the bare type name. clang and GCC both fully support the type, so
/// the pedantic diagnostic is silenced for this alias only; every other use
/// refers to `u128`.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
using u128 = unsigned __int128;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/// Largest representable u128 value, i.e. 2^128 - 1.
inline constexpr u128 u128_max = ~static_cast<u128>(0);

[[nodiscard]] constexpr u128 make_u128(std::uint64_t hi, std::uint64_t lo) noexcept {
    return (static_cast<u128>(hi) << 64) | static_cast<u128>(lo);
}

[[nodiscard]] constexpr std::uint64_t hi64(u128 x) noexcept {
    return static_cast<std::uint64_t>(x >> 64);
}

[[nodiscard]] constexpr std::uint64_t lo64(u128 x) noexcept {
    return static_cast<std::uint64_t>(x);
}

/// Count trailing zero bits. By convention ctz(0) == 128 (no set bit).
[[nodiscard]] constexpr int ctz(u128 x) noexcept {
    if (x == 0) {
        return 128;
    }
    const std::uint64_t low = lo64(x);
    if (low != 0) {
        return __builtin_ctzll(low);
    }
    return 64 + __builtin_ctzll(hi64(x));
}

/// Count leading zero bits. By convention clz(0) == 128 (no set bit).
[[nodiscard]] constexpr int clz(u128 x) noexcept {
    if (x == 0) {
        return 128;
    }
    const std::uint64_t high = hi64(x);
    if (high != 0) {
        return __builtin_clzll(high);
    }
    return 64 + __builtin_clzll(lo64(x));
}

/// Number of significant bits: position of the highest set bit + 1, so
/// bit_length(0) == 0 (clz(0) == 128).
[[nodiscard]] constexpr int bit_length(u128 x) noexcept {
    return 128 - clz(x);
}

// Checked arithmetic: each function returns the wrapped result and sets
// `overflow` iff the true result does not fit in 128 bits.

/// Returns a + b (mod 2^128); overflow set on carry-out.
[[nodiscard]] constexpr u128 checked_add(u128 a, u128 b, bool& overflow) noexcept {
    u128 result = 0;
    overflow = __builtin_add_overflow(a, b, &result);
    return result;
}

/// Returns a * b (mod 2^128); overflow set when the full product exceeds
/// 2^128 - 1.
[[nodiscard]] constexpr u128 checked_mul(u128 a, u128 b, bool& overflow) noexcept {
    u128 result = 0;
    overflow = __builtin_mul_overflow(a, b, &result);
    return result;
}

/// Formats a u128 as a base-10 string. "0" for zero.
[[nodiscard]] inline std::string to_string(u128 x) {
    if (x == 0) {
        return "0";
    }
    // At most 39 decimal digits for 2^128 - 1.
    char buf[40];
    char* p = buf + sizeof(buf);
    while (x != 0) {
        const auto digit = static_cast<unsigned>(x % 10);
        *--p = static_cast<char>('0' + digit);
        x /= 10;
    }
    return std::string(p, buf + sizeof(buf));
}

/// Parses a base-10 string into a u128. Returns true on success. On overflow or
/// invalid input, returns false and leaves `out` unspecified. Accepts an
/// optional leading '+'; rejects empty strings and non-digit characters.
[[nodiscard]] inline bool from_string(std::string_view s, u128& out) {
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    u128 value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        bool ovf = false;
        value = checked_mul(value, static_cast<u128>(10), ovf);
        if (ovf) {
            return false;
        }
        value = checked_add(value, static_cast<u128>(c - '0'), ovf);
        if (ovf) {
            return false;
        }
    }
    out = value;
    return true;
}

/// Canonical base-10 form of a non-negative integer of any size: drops an
/// optional leading '+' and leading zeros ("000" -> "0"). Returns false on an
/// empty string or a non-digit character.
[[nodiscard]] inline bool canonical_decimal(std::string_view s, std::string& out) {
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    while (s.size() > 1 && s.front() == '0') {
        s.remove_prefix(1);
    }
    out.assign(s.data(), s.size());
    return true;
}

/// a < b for canonical decimal strings (see canonical_decimal), compared as
/// integers of any size: a shorter string is smaller, equal lengths compare
/// lexicographically.
[[nodiscard]] inline bool decimal_less(std::string_view a, std::string_view b) noexcept {
    return a.size() != b.size() ? a.size() < b.size() : a < b;
}

} // namespace collatz

#endif // COLLATZ_U128_HPP
