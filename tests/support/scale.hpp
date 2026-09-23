#ifndef COLLATZ_TESTS_SCALE_HPP
#define COLLATZ_TESTS_SCALE_HPP

// Test sizes for the doctest suite.
//
// A sanitizer build (ASan and UBSan, -fsanitize in the compile flags) slows the
// GMP and enumeration sweeps by one to two orders of magnitude. Tests write each
// size as `scaled(full, small)`: optimized builds use `full`, sanitizer builds
// use `small`. Only the sweep sizes change; every assertion still runs.

namespace collatz::test {

/// True when the binary is built with a sanitizer (ASan or UBSan).
/// tests/CMakeLists.txt defines COLLATZ_SANITIZED when the compile flags contain
/// `-fsanitize`; the compilers' own feature macros (clang's __has_feature, gcc's
/// __SANITIZE_ADDRESS__) are the fallback.
inline bool sanitized() noexcept {
#if defined(COLLATZ_SANITIZED)
    return true;
#elif defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

/// A sweep size: `full` in optimized builds, but `small` under sanitizers.
inline unsigned scaled(unsigned full, unsigned small) noexcept {
    return sanitized() ? small : full;
}

} // namespace collatz::test

#endif // COLLATZ_TESTS_SCALE_HPP
