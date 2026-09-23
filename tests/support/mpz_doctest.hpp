#ifndef COLLATZ_TESTS_MPZ_DOCTEST_HPP
#define COLLATZ_TESTS_MPZ_DOCTEST_HPP

// doctest stringifies CHECK operands for failure messages. Its default path
// routes unknown types through `operator<<(std::ostream&, ...)`, which for
// mpz_class lives in libgmpxx and has the ABI of the C++ standard library
// libgmpxx was built with (see collatz/oracle.hpp). Specializing doctest's
// StringMaker to format via the GMP C API (see collatz::oracle::to_decimal)
// keeps every test binary off that symbol.

#include "collatz/oracle.hpp"

#include <doctest/doctest.h>
#include <gmpxx.h>

namespace doctest {

template <>
struct StringMaker<mpz_class> {
    static String convert(const mpz_class& value) {
        return String(collatz::oracle::to_decimal(value).c_str());
    }
};

} // namespace doctest

#endif // COLLATZ_TESTS_MPZ_DOCTEST_HPP
