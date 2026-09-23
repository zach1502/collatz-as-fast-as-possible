// The config file of `collatz worker` (collatz/worker/config.hpp): the round trip,
// malformed files, the worker token file and the state directory; also random ids,
// process liveness and the lock file (collatz/worker/process.hpp).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/worker.hpp"
#include "progress_lines.hpp"
#include "worker_helpers.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace wk = collatz::worker;
namespace fs = std::filesystem;
using collatz::test::own_pid;
using collatz::test::read_text;

TEST_CASE("config file: round trip and malformed files") {
    const fs::path dir = fs::temp_directory_path() / ("collatz-worker-test-" + wk::random_hex(6));
    const fs::path p = dir / "sub" / "worker.json";
    wk::Config c;
    std::string err;
    REQUIRE(wk::load_config(p, c, err)); // missing: empty and fine
    CHECK(c.host_id.empty());
    c.host_id = wk::random_hex(16);
    c.server = "https://x.example";
    REQUIRE_MESSAGE(wk::save_config(p, c, err), err);
    wk::Config d;
    REQUIRE_MESSAGE(wk::load_config(p, d, err), err);
    CHECK(d.host_id == c.host_id);
    CHECK(d.server == c.server);
    const fs::path shared = dir / "shared";
    fs::create_directories(shared);
    REQUIRE_MESSAGE(wk::save_config(shared / "w.json", c, err), err);
    unsigned leftovers = 0;
    for (const auto& e : fs::directory_iterator(shared)) {
        leftovers += e.path().filename().string().find(".tmp-") != std::string::npos ? 1u : 0u;
    }
    CHECK(leftovers == 0);
    std::ofstream(p) << R"({"host_id": "nothex"})";
    CHECK_FALSE(wk::load_config(p, d, err));
    std::ofstream(p) << "{truncated";
    CHECK_FALSE(wk::load_config(p, d, err));
    fs::remove_all(dir);
}

TEST_CASE("random ids and the worker lock") {
    const std::string h = wk::random_hex(32);
    CHECK(h.size() == 64);
    CHECK(h.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(wk::random_hex(32) != h);
    unsigned char buf[300] = {};
    REQUIRE(wk::random_bytes(buf, sizeof(buf)));
    CHECK(
        std::any_of(std::begin(buf) + 256, std::end(buf), [](unsigned char b) { return b != 0; }));
    CHECK(wk::process_alive(own_pid()));
    CHECK_FALSE(wk::process_alive(0));
    CHECK_FALSE(wk::process_alive(-5));

    const fs::path lock = fs::temp_directory_path() / ("collatz-worker-lock-" + wk::random_hex(6));
    std::string err;
    REQUIRE_MESSAGE(wk::acquire_lock(lock, err), err);
    CHECK(read_text(lock) == std::to_string(own_pid()) + "\n");
    // A second holder is refused while the lock is held, even in this process
    // (the old pid file let a process take over its own lock, and raced).
    CHECK_FALSE(wk::acquire_lock(lock, err));
    CHECK(err.find("process " + std::to_string(own_pid()) + " holds") != std::string::npos);
    wk::release_lock(lock);
    CHECK_MESSAGE(fs::exists(lock), "the file stays; the OS lock is what is released");
    REQUIRE_MESSAGE(wk::acquire_lock(lock, err), err);
    wk::release_lock(lock);
    // The pid in a file that no process holds the lock on does not matter: a
    // gone process, or a live one that reused the pid, leaves the lock free.
    std::ofstream(lock) << "2147483646\n";
    CHECK_MESSAGE(wk::acquire_lock(lock, err), err);
    wk::release_lock(lock);
    fs::remove(lock);
}

TEST_CASE("state directory: made by the worker with a marker; other directories refused") {
    const fs::path base = fs::temp_directory_path() / ("collatz-worker-state-" + wk::random_hex(6));
    const fs::path st = wk::state_dir(base / "w" / "worker.json");
    CHECK(st == base / "w" / "collatz-worker");
    std::string err;
    REQUIRE_MESSAGE(wk::open_state_dir(st, err), err);
    CHECK(fs::is_regular_file(st / wk::kStateMarker));
    std::ofstream(st / "worker.lock") << "1\n";
    REQUIRE_MESSAGE(wk::open_state_dir(st, err), err); // its own, with files: taken again
    // An empty directory is taken over; one with files but no marker is not.
    const fs::path empty = base / "e" / "collatz-worker";
    fs::create_directories(empty);
    REQUIRE_MESSAGE(wk::open_state_dir(empty, err), err);
    CHECK(fs::is_regular_file(empty / wk::kStateMarker));
    const fs::path foreign = base / "f" / "collatz-worker";
    fs::create_directories(foreign / "tmp");
    std::ofstream(foreign / "tmp" / "err-notes.txt") << "mine";
    CHECK_FALSE(wk::open_state_dir(foreign, err));
    CHECK(err.find("was not made by collatz worker") != std::string::npos);
    CHECK_FALSE(fs::exists(foreign / wk::kStateMarker));
    fs::create_directories(base / "m" / "collatz-worker" / wk::kStateMarker); // not a file
    std::ofstream(base / "m" / "collatz-worker" / "x") << "x";
    CHECK_FALSE(wk::open_state_dir(base / "m" / "collatz-worker", err));
    std::ofstream(base / "file") << "x";
    CHECK_FALSE(wk::open_state_dir(base / "file", err));
    fs::remove_all(base);
}

TEST_CASE("worker token files: trimmed, checked, and never quoted in an error") {
    const fs::path dir = fs::temp_directory_path() / ("collatz-worker-token-" + wk::random_hex(6));
    fs::create_directories(dir);
    const fs::path p = dir / "token.txt";
    const std::string tok = "0123456789abcdef0123456789abcdef-_.~XYZ";
    const auto read_back = [&p](const std::string& content, std::string& token, std::string& err) {
        std::ofstream(p, std::ios::binary) << content;
        return wk::read_worker_token(p, token, err);
    };
    std::string token;
    std::string err;
    REQUIRE_MESSAGE(read_back(tok + "\n", token, err), err);
    CHECK(token == tok);
    REQUIRE_MESSAGE(read_back(tok + "\r\n", token, err), err);
    CHECK(token == tok);
    REQUIRE_MESSAGE(read_back("\xEF\xBB\xBF  " + tok + " \t\r\n\n", token, err), err);
    CHECK(token == tok);

    const std::string secret_but_short = "abcdefghij0123456789";
    CHECK_FALSE(read_back(secret_but_short + "\n", token, err));
    CHECK(token.empty());
    CHECK(err.find(secret_but_short) == std::string::npos);
    CHECK(err.find("does not hold a worker token") != std::string::npos);
    CHECK_FALSE(read_back(tok + " second-word-0123456789abcdef\n", token, err));
    CHECK_FALSE(read_back(tok + "\n" + tok + "\n", token, err));
    CHECK_FALSE(read_back(tok + "\x01", token, err));
    CHECK_FALSE(read_back("", token, err));
    CHECK_FALSE(wk::read_worker_token(dir / "missing.txt", token, err));
    CHECK(err.find("cannot read") != std::string::npos);
    CHECK_FALSE(wk::read_worker_token(dir, token, err));

    CHECK_FALSE(wk::valid_worker_token(std::string(31, 'a')));
    CHECK(wk::valid_worker_token(std::string(32, 'a')));
    CHECK(wk::valid_worker_token(std::string(1024, 'a')));
    CHECK_FALSE(wk::valid_worker_token(std::string(1025, 'a')));
    CHECK_FALSE(wk::valid_worker_token(std::string(31, 'a') + " b"));
    CHECK_FALSE(wk::valid_worker_token(std::string(40, 'a') + "\xc3\xa9"));
    fs::remove_all(dir);
}
