#include "collatz/threading.hpp"

#include <thread>

namespace collatz {

unsigned hardware_threads() noexcept {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

} // namespace collatz
