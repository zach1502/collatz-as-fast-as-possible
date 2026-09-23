#include "collatz/oracle.hpp"

#include "collatz/u128.hpp"

#include <string>

namespace collatz::oracle {

mpz_class to_mpz(u128 n) {
    // GMP has no 128-bit setter, so the value goes through its decimal string.
    mpz_class result;
    const std::string dec = collatz::to_string(n);
    mpz_set_str(result.get_mpz_t(), dec.c_str(), 10);
    return result;
}

std::string to_decimal(const mpz_class& n) {
    // mpz_get_str allocates with GMP's allocator, so GMP's deallocator frees it.
    char* raw = mpz_get_str(nullptr, 10, n.get_mpz_t());
    std::string out(raw);
    void (*freefunc)(void*, std::size_t) = nullptr;
    mp_get_memory_functions(nullptr, nullptr, &freefunc);
    freefunc(raw, out.size() + 1);
    return out;
}

} // namespace collatz::oracle
