#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/oracle.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"
#include "mpz_doctest.hpp"
#include "scale.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <mutex>
#include <vector>

using collatz::u128;
using collatz::step1::Node;
using collatz::step1::Options;

namespace {

// Release builds use the full sizes; sanitizer builds use smaller ones so that
// the debug run stays within its time budget.
const unsigned kSmall = collatz::test::scaled(24, 16);
const unsigned kLarge = collatz::test::scaled(28, 18);

mpz_class step_T(const mpz_class& x) {
    if (mpz_odd_p(x.get_mpz_t())) {
        return (3 * x + 1) / 2;
    }
    return x / 2;
}

// Necessary condition for a class to still be undecided: its representative's
// trajectory never drops strictly below n0 within the first M steps.
void check_no_drop(unsigned M) {
    CAPTURE(M);
    std::uint64_t checked = 0;
    collatz::step1::enumerate(M, Options{}, [&](const Node& s) {
        const mpz_class n0 = collatz::oracle::to_mpz(s.n0);
        mpz_class x = n0;
        for (unsigned k = 1; k <= M; ++k) {
            x = step_T(x);
            if (x < n0) {
                REQUIRE_MESSAGE(false,
                                "survivor " << collatz::to_string(s.n0)
                                            << " drops below itself at step " << k);
            }
        }
        ++checked;
    });
    CHECK(checked > 0);
    MESSAGE("verified " << checked << " survivors never dip below n0 within " << M << " steps");
}

// The survivors sorted by n0: of the serial enumerate for threads == 1, else of
// a parallel run, whose sink runs concurrently, so a mutex guards the vector.
std::vector<Node> sorted_survivors(unsigned M, unsigned threads) {
    std::vector<Node> out;
    std::mutex mtx;
    const auto sink = [&](const Node& n) {
        std::lock_guard<std::mutex> lock(mtx);
        out.push_back(n);
    };
    if (threads == 1) {
        collatz::step1::enumerate(M, Options{}, sink);
    } else {
        collatz::step1::enumerate_parallel(M, threads, Options{}, sink);
    }
    std::sort(out.begin(), out.end(), [](const Node& a, const Node& b) { return a.n0 < b.n0; });
    return out;
}

} // namespace

TEST_CASE("survivors never drop below their start within M steps (GMP)") {
    check_no_drop(kSmall);
    check_no_drop(kLarge);
}

TEST_CASE("parallel enumeration equals serial (same sorted survivor set)") {
    for (const unsigned M : {kSmall, kLarge}) {
        CAPTURE(M);
        const std::vector<Node> serial = sorted_survivors(M, 1);
        const std::vector<Node> parallel = sorted_survivors(M, 8);
        REQUIRE(serial.size() == parallel.size());
        bool identical = true;
        for (std::size_t i = 0; i < serial.size(); ++i) {
            if (serial[i].n0 != parallel[i].n0 || serial[i].m != parallel[i].m ||
                serial[i].f != parallel[i].f) {
                identical = false;
                break;
            }
        }
        CHECK(identical);
    }
}
