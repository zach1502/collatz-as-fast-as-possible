#include "collatz/version.hpp"

namespace collatz {

std::string_view version_string() noexcept {
    // Must match the project() version in CMakeLists.txt.
    return "0.1.0";
}

} // namespace collatz
