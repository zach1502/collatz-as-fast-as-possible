#pragma once

// Helpers shared by the `collatz worker` unit tests test_worker_config.cpp,
// test_worker_http.cpp and test_worker_lease.cpp in tests/core/: this process's
// id and a JSON document parsed from text.
#include "collatz/checkpoint.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <process.h> // _getpid
#include <string>

namespace collatz::test {

inline long long own_pid() {
    return static_cast<long long>(_getpid());
}

inline checkpoint::Json json_of(const std::string& text) {
    const std::optional<checkpoint::Json> j = checkpoint::parse_json(text);
    REQUIRE(j.has_value());
    return *j;
}

} // namespace collatz::test
