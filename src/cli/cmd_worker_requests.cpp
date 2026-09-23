// Requests to the server and the waits between them: Worker::call, the auth
// fields of a request body, --status (GET /api/stats), the pause check, nap
// and the backoff.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/worker.hpp"
#include "collatz/worker_display.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace collatz::cli::worker_detail {

Reply Worker::call(const std::string& method, const std::string& path, const std::string& body) {
    Reply r;
    const wk::HttpResponse h =
        wk::http_request(s_.curl, method, s_.server + path, body, tmp_dir_, 120);
    r.status = h.status;
    if (h.status == 0) {
        r.error = wk::printable(h.error);
        return r;
    }
    std::string perr;
    r.json = collatz::checkpoint::parse_json(h.body, &perr);
    if (!r.json || r.json->type != Json::Type::Object) {
        r.json.reset();
        r.error = "HTTP " + std::to_string(h.status) + " with a non-JSON body";
        return r;
    }
    r.error = wk::printable(wk::string_member(&*r.json, "error"));
    return r;
}

std::string Worker::auth_fields() const {
    return "\"token\":" + wk::json_quote(s_.token) + ",\"host\":" + wk::json_quote(cfg_.host_id);
}

int Worker::print_status() {
    const Reply r = call("GET", "/api/stats", "");
    if (r.status != 200 || !r.json) {
        std::fprintf(stderr,
                     "error: %s/api/stats: %s\n",
                     s_.server.c_str(),
                     r.status == 0 ? r.error.c_str()
                                   : ("HTTP " + std::to_string(r.status)).c_str());
        return kExitError;
    }
    for (const std::string& line : wk::stats_summary(*r.json)) {
        std::printf("%s\n", line.c_str());
    }
    return kExitDone;
}

bool Worker::stop_requested() const {
    std::error_code ec;
    return g_stop.load() > 0 || fs::exists(pause_file_, ec);
}

bool Worker::nap(unsigned ms, const std::string& status) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    status_ = status;
    status_until_ = until;
    status_countdown_ = true;
    bool stopped = false;
    while (Clock::now() < until) {
        if (stop_requested()) {
            stopped = true;
            break;
        }
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    status_countdown_ = false;
    hide_display();
    return !stopped && !stop_requested();
}

unsigned Worker::next_backoff() {
    backoff_ = backoff_ == 0 ? s_.backoff_ms : std::min(backoff_ * 2u, 15u * 60u * 1000u);
    return backoff_;
}

} // namespace collatz::cli::worker_detail
