#ifndef COLLATZ_REF_REF_STEP1_INTERNAL_HPP
#define COLLATZ_REF_REF_STEP1_INTERNAL_HPP

// Private declarations shared by the ref/ref_step1*.cpp pieces of the Step-1
// reference: the T map on big integers and the backward preimage chain search
// of the mod-3^t rule.

#include <gmpxx.h>

namespace collatz::ref::detail {

[[nodiscard]] inline mpz_class step_T(const mpz_class& x) {
    if (mpz_even_p(x.get_mpz_t())) {
        return x / 2;
    }
    return (3 * x + 1) / 2;
}

/// Best backward preimage chain found for a concrete value m (reference form of
/// the mod-3^t sieve). `A` is the reached ancestor value = (2^L m - K)/3^v.
struct RefChain {
    bool found = false;
    unsigned L = 0; ///< total backward steps
    unsigned v = 0; ///< odd backward steps (1 <= v <= t)
    mpz_class A;    ///< reached ancestor value
};

// Defined in ref_step1_chain.cpp.
void ref_explore(const mpz_class& value,
                 unsigned L,
                 unsigned v,
                 unsigned t,
                 unsigned depth_left,
                 RefChain& best);

} // namespace collatz::ref::detail

#endif // COLLATZ_REF_REF_STEP1_INTERNAL_HPP
