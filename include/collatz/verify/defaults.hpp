#ifndef COLLATZ_VERIFY_DEFAULTS_HPP
#define COLLATZ_VERIFY_DEFAULTS_HPP

// Defaults the CLI derives from N without a device: the Step-2 look-ahead
// depth B.

namespace collatz::verify {

/// Step-2 look-ahead depth B used when --B is not given: 24 below N=40, 30 for N
/// in [40, 43] (28 on the GPU, use_gpu), 31 from N=44. The CLI raises the GPU
/// value to 32 from N=48 when A is 12. test_core_verify_defaults checks this
/// table.
[[nodiscard]] constexpr unsigned auto_step2_B(unsigned N, bool use_gpu) noexcept {
    if (N >= 44u) {
        return 31u;
    }
    if (N >= 40u) {
        return use_gpu ? 28u : 30u;
    }
    return 24u;
}

} // namespace collatz::verify

#endif // COLLATZ_VERIFY_DEFAULTS_HPP
