#ifndef COLLATZ_VERSION_HPP
#define COLLATZ_VERSION_HPP

#include <string_view>

namespace collatz {

// Version of the collatz verifier, e.g. "0.1.0"; equal to the CMake project()
// version.
[[nodiscard]] std::string_view version_string() noexcept;

} // namespace collatz

#endif // COLLATZ_VERSION_HPP
