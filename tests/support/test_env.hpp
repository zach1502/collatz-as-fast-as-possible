#pragma once

// Environment-variable helpers for tests, on the CRT's _putenv_s;
// _putenv_s(name, "") removes the variable. No caller checks the return
// value, so these return void.
#include <cstdlib>

namespace collatz::test {

// Set NAME=VALUE, overwriting any existing value.
inline void set_env(const char* name, const char* value) {
    _putenv_s(name, value);
}

inline void unset_env(const char* name) {
    _putenv_s(name, "");
}

// Whether a unit test may use the on-disk bv cache: true only when
// COLLATZ_CACHE_DIR is set, which tests/CMakeLists.txt does for every ctest run
// (build/<preset>/tests/.bvcache, used only by tests). A test binary run
// outside ctest never touches %LOCALAPPDATA%\collatz; under ctest the 2^B dip and
// packed-bitvector build runs once per build tree and later runs load it.
// A cache hit is bit-identical to a fresh build (step2_cache stores exactly
// what build_level produces; test_core_step2_cache_filter checks this), so a test
// that compares two runs of the same Filter loses no coverage by reusing it.
inline bool scoped_bv_cache() {
    const char* e = std::getenv("COLLATZ_CACHE_DIR");
    return e != nullptr && e[0] != '\0';
}

} // namespace collatz::test
