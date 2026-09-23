#ifndef COLLATZ_THREADING_HPP
#define COLLATZ_THREADING_HPP

// The hardware-thread count that sizes the parallel verifier's pool. The work
// splits into disjoint n0 chunks; the pool itself lives in the verifier.

namespace collatz {

/// Number of hardware threads available, clamped to at least 1 (so callers can
/// use it directly as a pool size even when the query returns 0/unknown).
[[nodiscard]] unsigned hardware_threads() noexcept;

} // namespace collatz

#endif // COLLATZ_THREADING_HPP
