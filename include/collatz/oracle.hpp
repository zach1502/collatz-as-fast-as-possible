#ifndef COLLATZ_ORACLE_HPP
#define COLLATZ_ORACLE_HPP

// Exact conversions between u128 and GMP's mpz_class, for the GMP replays of
// Step 3 and the decimal output of values above 2^128.
//
// libgmpxx's iostream and std::string operators are not used: their ABI is
// that of the C++ standard library libgmpxx was built with, so a build with
// another standard library fails to link them. mpz_class arithmetic is
// header-only expression templates and links with either; string conversion
// goes through the GMP C API (see oracle.cpp).

#include "collatz/u128.hpp"

#include <gmpxx.h>
#include <string>

namespace collatz::oracle {

/// Converts a u128 to an mpz_class (exactly, through a decimal string).
[[nodiscard]] mpz_class to_mpz(u128 n);

/// Formats an mpz_class as a base-10 string with the GMP C API.
[[nodiscard]] std::string to_decimal(const mpz_class& n);

} // namespace collatz::oracle

#endif // COLLATZ_ORACLE_HPP
