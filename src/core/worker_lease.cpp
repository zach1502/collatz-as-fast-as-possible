// The lease of a unit (parse_lease), the verify and certify command lines for
// it, the members that name it in a request, the files a unit keeps for it,
// and the choice of the units that are audited.

#include "collatz/preimage_chain.hpp"
#include "collatz/sha256.hpp"
#include "collatz/step2/params.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify/bounds.hpp"
#include "collatz/worker.hpp"
#include "worker_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace collatz::worker {

using checkpoint::Json;
using namespace detail;

namespace {

bool lease_uint(
    const Json& l, const char* key, std::uint64_t max, std::uint64_t& out, std::string& err) {
    const Json* v = l.get(key);
    u128 x = 0;
    if (v == nullptr || v->type != Json::Type::Number || !is_decimal(v->text) ||
        !collatz::from_string(v->text, x) || x > max) {
        err = std::string("lease.") + key + " is missing or out of range";
        return false;
    }
    out = static_cast<std::uint64_t>(x);
    return true;
}

bool lease_small(
    const Json& l, const char* key, unsigned lo, unsigned hi, unsigned& out, std::string& err) {
    std::uint64_t v = 0;
    if (!lease_uint(l, key, hi, v, err) || v < lo) {
        err = std::string("lease.") + key + " is missing or outside [" + std::to_string(lo) + ", " +
              std::to_string(hi) + "]";
        return false;
    }
    out = static_cast<unsigned>(v);
    return true;
}

/// Plain text the worker will pass as one argv element: printable ASCII
/// without the characters a shell, cmd.exe or MinGW argv globbing would treat
/// specially (the campaign script enforces the same set).
bool plain_text(std::string_view s) {
    static constexpr std::string_view kExtra = " .,:;()'+=^/_-";
    if (s.empty() || s.size() > 200) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               kExtra.find(c) != std::string_view::npos;
    });
}

} // namespace

bool parse_lease(const Json& l, Lease& out, std::string& err) {
    out = Lease{};
    if (l.type != Json::Type::Object) {
        err = "lease is not an object";
        return false;
    }
    constexpr std::uint64_t kMaxId = (std::uint64_t{1} << 53) - 1;
    std::uint64_t expires = 0;
    if (!lease_uint(l, "unit_id", kMaxId, out.unit_id, err) ||
        !lease_uint(l, "campaign_id", kMaxId, out.campaign_id, err) ||
        !lease_uint(l, "lease_expires", kMaxId, expires, err) ||
        !lease_small(l, "k", 32, 96, out.K, err) || !lease_small(l, "a", 1, 31, out.A, err) ||
        !lease_small(l, "b", 2, step2::max_B, out.B, err) ||
        !lease_small(l, "bitvectors", 1, verify::kMaxBitvectors, out.bitvectors, err) ||
        !lease_small(l, "mod3t", 0, preimage::max_t, out.mod3t, err) ||
        !lease_small(l, "window_chains", 0, preimage::max_t, out.window_chains, err) ||
        !lease_small(
            l, "window_templates_d", 0, preimage::max_window_t, out.window_templates_d, err)) {
        return false;
    }
    out.lease_expires = static_cast<std::int64_t>(expires);
    // A server from before lease ids sends none; the requests then name the unit only.
    if (l.get("lease_id") != nullptr && !lease_uint(l, "lease_id", kMaxId, out.lease_id, err)) {
        return false;
    }
    if (out.A >= out.K) {
        err = "lease.a must be below lease.k";
        return false;
    }
    const Json* h = l.get("h");
    if (h == nullptr || h->type != Json::Type::String || !is_decimal(h->text) ||
        h->text.size() > 30) {
        err = "lease.h is not a decimal string";
        return false;
    }
    out.h = h->text;
    const Json* po = l.get("partial_oee");
    if (po == nullptr || po->type != Json::Type::Bool) {
        err = "lease.partial_oee is not a boolean";
        return false;
    }
    out.partial_oee = po->b;
    const Json* kern = l.get("kernel");
    if (kern == nullptr || kern->type != Json::Type::String || kern->text != "table16") {
        err = "lease.kernel is not a known Step-3 kernel";
        return false;
    }
    out.kernel = kern->text;
    const Json* thr = l.get("record_threshold");
    if (thr != nullptr && thr->type == Json::Type::Null) {
        out.record_threshold = "none";
    } else if (thr != nullptr && thr->type == Json::Type::String && is_decimal(thr->text) &&
               thr->text.size() <= 200) {
        out.record_threshold = thr->text;
    } else {
        err = "lease.record_threshold is neither null nor a decimal string";
        return false;
    }
    const Json* src = l.get("assumption_source");
    if (src != nullptr && src->type == Json::Type::String && plain_text(src->text)) {
        out.assumption_source = src->text;
    } else if (src == nullptr || src->type != Json::Type::Null) {
        err = "lease.assumption_source is not null or plain text";
        return false;
    }
    if (const Json* be = l.get("backend"); be != nullptr && be->type != Json::Type::Null) {
        if (be->type != Json::Type::String || (be->text != "gpu" && be->text != "cpu")) {
            err = "lease.backend is not \"gpu\" or \"cpu\"";
            return false;
        }
        out.backend = be->text;
    }
    if (const Json* n = l.get("note"); n != nullptr && n->type != Json::Type::Null) {
        if (n->type != Json::Type::String || n->text.size() > 400) {
            err = "lease.note is not a string of at most 400 bytes";
            return false;
        }
        out.note = n->text;
    }
    return true;
}

bool lease_uses_gpu(const Lease& l, bool worker_gpu) noexcept {
    return worker_gpu && l.backend != "cpu";
}

std::string lease_fields(const Lease& l) {
    std::string s = "\"unit_id\":" + std::to_string(l.unit_id);
    if (l.lease_id != 0) {
        s += ",\"lease_id\":" + std::to_string(l.lease_id);
    }
    return s;
}

void keep_unit_files(const std::filesystem::path& udir, const Lease& l) {
    const std::filesystem::path marker = udir / "lease-id";
    const std::string id = std::to_string(l.lease_id);
    std::string kept;
    if (read_text(marker, kept)) {
        while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r')) {
            kept.pop_back();
        }
        if (kept == id) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(udir / "ck", ec);
    std::filesystem::remove(udir / "result.json", ec);
    // Unwritten, the next call removes the files again: the unit runs from scratch.
    std::string err;
    (void) checkpoint::write_file_atomic(marker.string(), id + "\n", err);
}

std::vector<std::string> verify_args(const Lease& l,
                                     unsigned threads,
                                     bool gpu,
                                     const std::string& checkpoint_dir,
                                     const std::string& json_path,
                                     const std::string& progress_path) {
    const auto u = [](unsigned v) { return std::to_string(v); };
    std::vector<std::string> a = {"verify",
                                  "--block",
                                  l.h,
                                  "--K",
                                  u(l.K),
                                  "--A",
                                  u(l.A),
                                  "--B",
                                  u(l.B),
                                  "--bitvectors",
                                  u(l.bitvectors),
                                  "--kernel",
                                  l.kernel,
                                  "--mod3t",
                                  u(l.mod3t),
                                  "--window-chains",
                                  u(l.window_chains),
                                  "--window-templates-D",
                                  u(l.window_templates_d)};
    if (!l.partial_oee) {
        a.emplace_back("--no-partial-oee");
    }
    a.insert(a.end(), {"--record-threshold", l.record_threshold});
    if (!l.assumption_source.empty()) {
        a.insert(a.end(), {"--assumption-source", l.assumption_source});
    }
    a.insert(a.end(), {"--threads", u(threads)});
    if (gpu) {
        a.emplace_back("--gpu");
    }
    a.insert(a.end(), {"--checkpoint", checkpoint_dir, "--json", json_path});
    if (!progress_path.empty()) {
        a.insert(a.end(), {"--progress-file", progress_path});
    }
    return a;
}

std::vector<std::string> audit_args(const Lease& l,
                                    const std::string& seed,
                                    unsigned threads,
                                    const std::string& result_path,
                                    const std::string& json_path) {
    const auto u = [](unsigned v) { return std::to_string(v); };
    std::vector<std::string> a = {"certify",
                                  "--block",
                                  l.h,
                                  "--K",
                                  u(l.K),
                                  "--A",
                                  u(l.A),
                                  "--B",
                                  u(l.B),
                                  "--bitvectors",
                                  u(l.bitvectors),
                                  "--mod3t",
                                  u(l.mod3t),
                                  "--window-chains",
                                  u(l.window_chains),
                                  "--window-templates-D",
                                  u(l.window_templates_d)};
    if (!l.partial_oee) {
        a.emplace_back("--no-partial-oee");
    }
    a.insert(a.end(),
             {"--threads",
              u(threads),
              "--audit-seed",
              seed,
              "--result",
              result_path,
              "--json",
              json_path,
              "--audit-cache-only"});
    return a;
}

bool audit_selected(std::string_view digest, double rate) noexcept {
    if (!(rate > 0.0)) {
        return false; // also NaN
    }
    if (rate >= 1.0) {
        return true;
    }
    Sha256 h;
    h.update("collatz-audit-select|");
    h.update(digest);
    const std::array<std::uint8_t, 32> d = h.finish();
    std::uint64_t x = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        x = (x << 8) | static_cast<std::uint64_t>(d[i]);
    }
    // rate * 2^64 < 2^64 here; ldexp is exact and the cast truncates.
    return x < static_cast<std::uint64_t>(std::ldexp(rate, 64));
}

} // namespace collatz::worker
