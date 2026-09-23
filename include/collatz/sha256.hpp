#ifndef COLLATZ_SHA256_HPP
#define COLLATZ_SHA256_HPP

// SHA-256 (FIPS 180-4) in portable C++ with no external dependency, for the
// work-unit result digest. The inputs are public, so the implementation makes
// no constant-time claim.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace collatz {

/// Incremental SHA-256: update() any number of times, then finish() once.
class Sha256 {
public:
    Sha256() noexcept;
    void update(const void* data, std::size_t len) noexcept;
    void update(std::string_view s) noexcept {
        update(s.data(), s.size());
    }
    /// The 32-byte digest. The object must not be updated after this.
    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_bytes_ = 0;
};

/// Lowercase hex SHA-256 of `s` (64 characters).
[[nodiscard]] std::string sha256_hex(std::string_view s);

} // namespace collatz

#endif // COLLATZ_SHA256_HPP
