#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/sha256.hpp"

#include <cstddef>
#include <doctest/doctest.h>
#include <string>

TEST_CASE("sha256 matches the FIPS 180-4 test vectors") {
    CHECK(collatz::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(collatz::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(collatz::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million 'a', fed in uneven chunks (exercises the block buffering).
    collatz::Sha256 h;
    const std::string chunk(997, 'a');
    std::size_t left = 1000000;
    while (left > 0) {
        const std::size_t n = left < chunk.size() ? left : chunk.size();
        h.update(chunk.data(), n);
        left -= n;
    }
    const auto d = h.finish();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (const auto b : d) {
        hex += kHex[b >> 4];
        hex += kHex[b & 0x0Fu];
    }
    CHECK(hex == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
