// `collatz worker` HTTP through the system curl executable and the child
// processes it runs (collatz/worker/http.hpp, collatz/worker/process.hpp): Windows
// argument quoting, curl's status line, the rule for server URLs, stale request
// files and audit directories, and exit codes as text.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "collatz/worker.hpp"
#include "worker_helpers.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace wk = collatz::worker;
namespace fs = std::filesystem;
using collatz::test::own_pid;

TEST_CASE("quote_windows_arg follows the CommandLineToArgvW rules") {
    CHECK(wk::quote_windows_arg("plain") == "plain");
    CHECK(wk::quote_windows_arg("C:\\dir\\file") == "C:\\dir\\file");
    CHECK(wk::quote_windows_arg("") == "\"\"");
    CHECK(wk::quote_windows_arg("a b") == "\"a b\"");
    CHECK(wk::quote_windows_arg("a\"b") == "\"a\\\"b\"");
    CHECK(wk::quote_windows_arg("C:\\with space\\") == "\"C:\\with space\\\\\"");
    CHECK(wk::quote_windows_arg("a\\\\\"b") == "\"a\\\\\\\\\\\"b\"");
    CHECK(wk::quote_windows_arg("x\\ y") == "\"x\\ y\"");
}

TEST_CASE("parse_http_status takes the last status line") {
    CHECK(wk::parse_http_status("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nX: y\r\n") ==
          201);
    CHECK(wk::parse_http_status("HTTP/2 404 \r\ncontent-type: x\r\n") == 404);
    CHECK(wk::parse_http_status("HTTP/1.1 2x0 Bad\r\n") == 0);
    CHECK(wk::parse_http_status("") == 0);
}

TEST_CASE("server_url_ok: https anywhere, http only to this machine") {
    CHECK(wk::server_url_ok("https://collatz.example.workers.dev"));
    CHECK(wk::server_url_ok("HTTPS://X.ORG/path"));
    CHECK(wk::server_url_ok("http://localhost:8787"));
    CHECK(wk::server_url_ok("http://127.0.0.1"));
    CHECK(wk::server_url_ok("http://[::1]:9000/"));
    CHECK_FALSE(wk::server_url_ok("http://example.com"));
    CHECK_FALSE(wk::server_url_ok("http://localhost.evil.example"));
    CHECK_FALSE(wk::server_url_ok("http://127.0.0.1.evil.example"));
    CHECK_FALSE(wk::server_url_ok("ftp://x"));
    CHECK_FALSE(wk::server_url_ok("https://"));
    CHECK_FALSE(wk::server_url_ok("https:///x"));
    CHECK_FALSE(wk::server_url_ok("https://a b"));
    CHECK_FALSE(wk::server_url_ok("file:///C:/Windows/win.ini"));
}

TEST_CASE("remove_stale_request_files removes only its own names of gone processes") {
    const fs::path dir = fs::temp_directory_path() / ("collatz-worker-tmp-" + wk::random_hex(6));
    fs::create_directories(dir);
    const std::string live = std::to_string(own_pid());
    const std::vector<std::string> names = {"req-2147483646-1-0123abcd.json",
                                            "resp-2147483646-1-0123abcd.json",
                                            "hdr-2147483646-1-0123abcd.txt",
                                            "err-2147483646-1-0123abcd.txt",
                                            "req-" + live + "-1-89abcdef.json",
                                            "notes.txt"};
    // A user's files that only start like the worker's: kept.
    const std::vector<std::string> foreign = {"err-notes.txt",
                                              "req-important.doc",
                                              "hdr-x",
                                              "req-2147483646-1-0123abcd.doc",
                                              "hdr-2147483646-1-0123abcd.json",
                                              "resp-2147483646-9-0123ABCD.json",
                                              "err-2147483646-1-012.txt",
                                              "req-2147483646-x-0123abcd.json",
                                              "req--1-0123abcd.json"};
    for (const std::string& name : names) {
        std::ofstream(dir / name) << "x";
    }
    for (const std::string& name : foreign) {
        std::ofstream(dir / name) << "x";
    }
    CHECK(wk::remove_stale_request_files(dir) == 4);
    CHECK(fs::exists(dir / ("req-" + live + "-1-89abcdef.json")));
    CHECK(fs::exists(dir / "notes.txt"));
    CHECK_FALSE(fs::exists(dir / "req-2147483646-1-0123abcd.json"));
    for (const std::string& name : foreign) {
        CHECK_MESSAGE(fs::exists(dir / name), name);
    }
    fs::remove_all(dir);
}

TEST_CASE("remove_stale_audit_dirs keeps failed audits, unsent outcomes and others' files") {
    const fs::path dir = fs::temp_directory_path() / ("collatz-worker-audits-" + wk::random_hex(6));
    for (const char* unit : {"11", "12", "13", "14", "15", "notes", "016"}) {
        fs::create_directories(dir / unit);
        std::ofstream(dir / unit / "result.json") << "{}";
    }
    std::ofstream(dir / "12" / "audit.json") << "{\"failures_total\": 0, \"seconds\": 1.5}";
    std::ofstream(dir / "13" / "audit.json") << "{\"failures_total\": 3, \"seconds\": 1.5}";
    std::ofstream(dir / "14" / "audit.json") << "{\"failures_total\": "; // cut off
    std::ofstream(dir / "15" / "audit.json") << "{\"failures_total\": 0, \"seconds\": 1.5}";
    std::ofstream(dir / "15" / wk::kUnreportedAudit) << "{}";
    std::ofstream(dir / "notes.txt") << "x";
    CHECK(wk::remove_stale_audit_dirs(dir) == 3);
    CHECK(fs::exists(dir / "13" / "audit.json"));
    CHECK(fs::exists(dir / "15" / wk::kUnreportedAudit));
    CHECK_FALSE(fs::exists(dir / "11"));
    CHECK_FALSE(fs::exists(dir / "12"));
    CHECK_FALSE(fs::exists(dir / "14"));
    CHECK(fs::exists(dir / "notes.txt"));
    CHECK_MESSAGE(fs::exists(dir / "notes" / "result.json"), "not a unit id: not the worker's");
    CHECK_MESSAGE(fs::exists(dir / "016" / "result.json"), "not a unit id: not the worker's");
    CHECK(wk::remove_stale_audit_dirs(dir / "missing") == 0);
    fs::remove_all(dir);
}

TEST_CASE("exit_code_text: decimal, and an NTSTATUS in hex") {
    CHECK(wk::exit_code_text(0) == "0");
    CHECK(wk::exit_code_text(137) == "137");
    CHECK(wk::exit_code_text(65535) == "65535");
    CHECK(wk::exit_code_text(65536) == "0x00010000");
    CHECK(wk::exit_code_text(-1) == "0xFFFFFFFF");
    // STATUS_ACCESS_VIOLATION as GetExitCodeProcess gives it and run_process returns it.
    CHECK(wk::exit_code_text(static_cast<int>(0xC0000005u)) == "0xC0000005");
}
