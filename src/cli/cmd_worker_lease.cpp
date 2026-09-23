// The unit directories and their lease.json, the check that verify can run a
// lease's block, and the lease requests POST /api/fetch and POST /api/release.

#include "cmd_worker_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/step2/params.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify/bounds.hpp"
#include "collatz/verify/options.hpp"
#include "collatz/worker.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace collatz::cli::worker_detail {

bool lease_runnable(const wk::Lease& l, std::string& why) {
    collatz::u128 h = 0;
    if (!collatz::from_string(l.h, h) || h == 0) {
        why = "lease.h is 0, which verify --block would take as a full run of [0, 2^" +
              std::to_string(l.K) + ")";
        return false;
    }
    if (l.A > collatz::step2::max_A || l.K > 76u + l.A) {
        why = "verify needs lease.a <= " + std::to_string(collatz::step2::max_A) +
              " and lease.k <= 76 + lease.a (got a=" + std::to_string(l.A) +
              ", k=" + std::to_string(l.K) + ")";
        return false;
    }
    // The range checks of verify itself: start >= 2^17, end <= 2^96, the u128 lift.
    collatz::verify::Options o;
    o.N = l.K;
    o.A = l.A;
    o.B = l.B;
    o.block_h = h;
    try {
        collatz::verify::validate_block(o);
    } catch (const std::invalid_argument& e) {
        why = e.what();
        return false;
    }
    return true;
}

std::optional<fs::path> Worker::pending_unit() const {
    std::error_code ec;
    std::optional<fs::path> best;
    std::uint64_t best_id = ~std::uint64_t{0};
    for (const auto& e : fs::directory_iterator(units_dir_, ec)) {
        std::uint64_t id = 0;
        if (!e.is_directory(ec) || !parse_u64(e.path().filename().string(), id) ||
            !fs::exists(e.path() / "lease.json", ec)) {
            continue;
        }
        if (id < best_id) {
            best_id = id;
            best = e.path();
        }
    }
    return best;
}

std::optional<wk::Lease> Worker::load_lease(const fs::path& udir, bool& released) const {
    std::string text;
    if (!read_file(udir / "lease.json", text)) {
        return std::nullopt;
    }
    const std::optional<Json> doc = collatz::checkpoint::parse_json(text);
    if (!doc) {
        return std::nullopt;
    }
    const Json* lease = doc->get("lease");
    wk::Lease l;
    std::string err;
    if (lease == nullptr || !wk::parse_lease(*lease, l, err)) {
        return std::nullopt;
    }
    const Json* rel = doc->get("released");
    released = rel != nullptr && rel->type == Json::Type::Bool && rel->b;
    if (wk::string_member(&*doc, "binary_commit") != COLLATZ_GIT_COMMIT) {
        // Slices from another build would make verify refuse the resume (and
        // could not be submitted under this commit): start the unit over.
        std::error_code ec;
        fs::remove_all(udir / "ck", ec);
        fs::remove(udir / "result.json", ec);
        if (!save_lease(udir, *lease, released)) {
            return std::nullopt;
        }
    }
    return l;
}

bool Worker::save_lease(const fs::path& udir, const Json& lease, bool released) const {
    std::error_code ec;
    fs::create_directories(udir, ec);
    const std::string text = "{\"schema\":\"collatz-worker-lease/1\",\"binary_commit\":" +
                             wk::json_quote(COLLATZ_GIT_COMMIT) +
                             ",\"released\":" + (released ? "true" : "false") +
                             ",\"lease\":" + wk::json_dump(lease) + "}\n";
    std::string err;
    if (!collatz::checkpoint::write_file_atomic((udir / "lease.json").string(), text, err)) {
        std::fprintf(stderr, "worker: %s\n", err.c_str());
        return false;
    }
    return true;
}

void Worker::drop(const fs::path& udir) const {
    std::error_code ec;
    fs::remove_all(udir, ec);
}

Worker::Got Worker::fetch(wk::Lease& out, fs::path& udir, std::optional<std::uint64_t> prefer) {
    std::string body =
        "{" + auth_fields() + ",\"commit\":" + wk::json_quote(COLLATZ_GIT_COMMIT) +
        ",\"digest_schema\":" + wk::json_quote(collatz::checkpoint::kResultDigestSchema) +
        ",\"bits\":" + std::to_string(s_.block_bits) + ",\"backend\":\"" +
        (s_.gpu ? "gpu" : "cpu") +
        "\",\"recheck_cpu\":" + (s_.gpu && s_.recheck_cpu ? "true" : "false");
    if (prefer) {
        body += ",\"prefer_unit\":" + std::to_string(*prefer);
    }
    body += '}';
    const Reply r = call("POST", "/api/fetch", body);
    no_work_ = false;
    const auto wait_for = [this](const Reply& rr, std::uint64_t fallback_s) {
        std::uint64_t secs = fallback_s;
        (void) parse_u64(rr.json ? num_of(&*rr.json, "retry_after") : std::string(), secs);
        backoff_ = static_cast<unsigned>(std::clamp<std::uint64_t>(secs, 10, 3600) * 1000);
        return Got::Wait;
    };
    if (r.status == 200 && r.json) {
        backoff_ = 0;
        const Json* lease = r.json->get("lease");
        if (lease == nullptr || lease->type == Json::Type::Null) {
            wait_reason_ = wk::printable(wk::string_member(&*r.json, "reason"));
            no_work_ = true;
            return wait_for(r, 600);
        }
        std::string err;
        if (!wk::parse_lease(*lease, out, err) || !lease_runnable(out, err)) {
            std::fprintf(stderr, "error: the server sent an unusable lease (%s)\n", err.c_str());
            return Got::Fatal;
        }
        udir = units_dir_ / std::to_string(out.unit_id);
        if (out.K > s_.block_bits) {
            // Never run a block larger than --block-bits. A live lease handed
            // out again or a renewed one may be from a session with a larger
            // --block-bits: hand it back and ask again. A new block this large
            // means the campaign has no smaller ones.
            const Json* again = r.json->get("redelivered");
            const bool held = (again != nullptr && again->type == Json::Type::Bool && again->b) ||
                              (prefer && out.unit_id == *prefer);
            release(out, udir);
            drop(udir);
            if (!held) {
                std::fprintf(stderr,
                             "error: this campaign's smallest blocks are 2^%u numbers, more than "
                             "--block-bits %u; run with --block-bits %u or more\n",
                             out.K,
                             s_.block_bits,
                             out.K);
                if (!out.note.empty()) {
                    std::fprintf(
                        stderr, "  the server says: %s\n", wk::printable(out.note).c_str());
                }
                return Got::Fatal;
            }
            std::printf("worker: handed back unit %llu, a block of 2^%u numbers (more than "
                        "--block-bits %u); asking for a smaller one\n",
                        static_cast<unsigned long long>(out.unit_id),
                        out.K,
                        s_.block_bits);
            std::fflush(stdout);
            wait_reason_ = "handed back a block larger than --block-bits";
            backoff_ = 1000;
            return Got::Wait;
        }
        if (out.K != s_.block_bits || !out.note.empty()) {
            std::printf("worker: unit %llu is a block of 2^%u numbers (asked for 2^%u)%s%s\n",
                        static_cast<unsigned long long>(out.unit_id),
                        out.K,
                        s_.block_bits,
                        out.note.empty() ? "" : ": ",
                        wk::printable(out.note).c_str());
            std::fflush(stdout);
        }
        return save_lease(udir, *lease, false) ? Got::Lease : Got::Fatal;
    }
    // 404: --server has no /api/fetch, so it is no work server.
    if (r.status == 401 || r.status == 403 || r.status == 400 || r.status == 404 ||
        r.status == 410) {
        std::fprintf(stderr, "error: %s\n", reply_error(r).c_str());
        if (r.json) {
            if (const Json* a = r.json->get("allowed_commits");
                a != nullptr && a->type == Json::Type::Array) {
                std::fprintf(stderr, "  this binary: %s\n  allowed:", COLLATZ_GIT_COMMIT);
                for (const Json& c : a->arr) {
                    std::fprintf(stderr, " %s", wk::printable(c.text).c_str());
                }
                std::fprintf(stderr, "\n  build one of them (git checkout COMMIT, then rebuild)\n");
            }
        }
        return Got::Fatal;
    }
    wait_reason_ = r.status == 0 ? r.error
                                 : "HTTP " + std::to_string(r.status) +
                                       (r.error.empty() ? "" : ": " + r.error);
    if (r.status == 429 || r.status == 503) {
        return wait_for(r, 60);
    }
    (void) next_backoff();
    return Got::Wait;
}

void Worker::release(const wk::Lease& l, const fs::path& udir) {
    const std::string body = "{" + auth_fields() + "," + wk::lease_fields(l) + "}";
    const Reply r = call("POST", "/api/release", body);
    if (r.status != 200) {
        std::fprintf(stderr,
                     "worker: could not hand unit %llu back (%s); its lease expires on its own\n",
                     static_cast<unsigned long long>(l.unit_id),
                     reply_error(r).c_str());
    }
    // Keep the finished slices: the next start asks for this unit first.
    (void) mark_released(udir);
}

bool Worker::mark_released(const fs::path& udir) const {
    std::string text;
    if (read_file(udir / "lease.json", text)) {
        if (const std::optional<Json> doc = collatz::checkpoint::parse_json(text)) {
            if (const Json* lease = doc->get("lease")) {
                return save_lease(udir, *lease, true);
            }
        }
    }
    return false;
}

} // namespace collatz::cli::worker_detail
