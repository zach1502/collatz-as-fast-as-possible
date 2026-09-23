// POST /api/submit of a unit's result.json and the server's verdict.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/sha256.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace collatz::cli::worker_detail {

namespace {

/// Replaces the text of member `key` of `obj` (which must exist).
bool set_member(Json& obj, const char* key, const std::string& text) {
    for (auto& kv : obj.obj) {
        if (kv.first == key) {
            kv.second.text = text;
            return true;
        }
    }
    return false;
}

Json* member(Json& obj, const char* key) {
    for (auto& kv : obj.obj) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

} // namespace

Worker::Unit Worker::submit(const wk::Lease& l, const fs::path& udir, double seconds) {
    std::string text;
    std::string err;
    std::optional<Json> doc;
    if (read_file(udir / "result.json", text)) {
        doc = collatz::checkpoint::parse_json(text, &err);
    }
    std::string dt;
    if (!doc || !wk::digest_text_from_json(*doc, dt, err) ||
        collatz::sha256_hex(dt) != wk::string_member(doc->get("result"), "result_digest")) {
        std::fprintf(stderr,
                     "worker: unit %llu: result.json is unreadable or its digest does not match "
                     "(%s); running it again\n",
                     static_cast<unsigned long long>(l.unit_id),
                     err.c_str());
        std::error_code ec;
        fs::remove(udir / "result.json", ec);
        fs::remove_all(udir / "ck", ec);
        return Unit::Failed;
    }
    // The hostname stays on this machine.
    if (Json* machine = member(*doc, "machine"); machine != nullptr) {
        (void) set_member(*machine, "host", "");
    }
    // The audit seed: the digest verify wrote.
    const std::string digest = wk::string_member(doc->get("result"), "result_digest");
    const std::string body = "{" + auth_fields() + "," + wk::lease_fields(l) +
                             ",\"result_json\":" + wk::json_dump(*doc) + "}";
    unsigned tries = 0; // failed submits, bounded with --once
    for (;;) {
        const Reply r = call("POST", "/api/submit", body);
        const double log2_start =
            std::log2(std::strtod(l.h.c_str(), nullptr)) + static_cast<double>(l.K);
        const bool on_cpu = s_.gpu && !wk::lease_uses_gpu(l, s_.gpu);
        char head[200];
        std::snprintf(head,
                      sizeof(head),
                      "unit %llu (2^%u numbers at 2^%.4f%s) %.1f s",
                      static_cast<unsigned long long>(l.unit_id),
                      l.K,
                      log2_start,
                      on_cpu ? ", on the CPU" : "",
                      seconds);
        if (r.status == 200 && r.json) {
            backoff_ = 0;
            const std::string state = wk::string_member(&*r.json, "state");
            const std::string verdict = wk::string_member(&*r.json, "verdict");
            const Json* dup = r.json->get("duplicate");
            std::string what;
            const bool duplicate = dup != nullptr && dup->type == Json::Type::Bool && dup->b;
            if (duplicate) {
                what = "already submitted";
            } else if (verdict == "accepted") {
                what = "accepted";
            } else if (state == "conflict") {
                what = "submitted; the unit's results disagree, so it gets another run";
            } else {
                what = "submitted";
            }
            std::printf("%s: %s\n", head, what.c_str());
            std::fflush(stdout);
            log_line(std::string(head) + ": " + what);
            ++session_blocks_;
            session_numbers_ += pending_numbers_;
            pending_numbers_ = 0.0;
            last_ = "unit " + std::to_string(l.unit_id) + " " + what + " at " +
                    wk::local_time_text(std::time(nullptr), false) +
                    (seconds > 0.0 ? " after " + wk::format_duration(seconds) : std::string());
            if (!duplicate && verdict == "accepted") {
                queue_audit(l, udir, digest);
            }
            drop(udir);
            return Unit::Done;
        }
        const std::string lease_state =
            r.json ? wk::string_member(&*r.json, "lease_state") : std::string();
        if (r.status == 409 && !lease_state.empty()) {
            // Only a live lease is accepted: ask for the unit again and resend
            // the kept result if the server hands it back.
            std::printf("%s: the lease %s before the result arrived; asking for the unit again\n",
                        head,
                        lease_state == "released" ? "was handed back" : "expired");
            std::fflush(stdout);
            return mark_released(udir) ? Unit::Renew : Unit::Failed;
        }
        if (r.status == 401 || r.status == 403) {
            // The worker token was refused, not the result: keep the result
            // for the next run.
            std::fprintf(stderr,
                         "%s: the server refused the worker token: %s\n",
                         head,
                         reply_error(r).c_str());
            return Unit::TokenRefused;
        }
        if (r.status >= 400 && r.status < 500 && r.status != 429) {
            const std::string why = reply_error(r);
            std::fprintf(stderr, "%s: rejected by the server: %s\n", head, why.c_str());
            log_line(std::string(head) + ": rejected by the server: " + why);
            last_ = "unit " + std::to_string(l.unit_id) + " rejected by the server at " +
                    wk::local_time_text(std::time(nullptr), false) + " (see above)";
            drop(udir);
            return Unit::Rejected;
        }
        const std::string why = r.status == 0 ? r.error : "HTTP " + std::to_string(r.status);
        if (s_.max_units == 1 && ++tries >= kOnceTries) {
            std::fprintf(stderr,
                         "error: cannot submit unit %llu after %u tries (%s); its result is "
                         "kept: run the same command to submit it\n",
                         static_cast<unsigned long long>(l.unit_id),
                         kOnceTries,
                         why.c_str());
            return Unit::Unsent;
        }
        const unsigned wait = next_backoff();
        std::fprintf(stderr,
                     "worker: cannot submit unit %llu (%s); retrying in %u s\n",
                     static_cast<unsigned long long>(l.unit_id),
                     why.c_str(),
                     wait / 1000);
        if (!nap(wait,
                 "cannot submit unit " + std::to_string(l.unit_id) + "; the result is kept")) {
            return Unit::Paused; // result.json stays; the next start submits it
        }
    }
}

} // namespace collatz::cli::worker_detail
