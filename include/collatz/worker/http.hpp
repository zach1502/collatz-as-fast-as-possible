#ifndef COLLATZ_WORKER_HTTP_HPP
#define COLLATZ_WORKER_HTTP_HPP

// HTTP through the system curl executable: http_request, the request files it
// leaves, curl's header dump, and the rule for server URLs.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace collatz::worker {

struct HttpResponse {
    int status = 0; ///< HTTP status; 0 = no response (network error, see `error`)
    std::string body;
    std::string error;
};

/// One HTTP request through the curl executable `curl`, run as `curl -q` (no
/// .curlrc), restricted to https (http only for a localhost, 127.0.0.1 or
/// [::1] URL), following no redirects. A POST body is JSON; it is written to
/// an owner-only file in `tmp_dir` and passed as --data-binary @file, so the
/// worker token inside it never appears on a command line; the file and
/// curl's response files are deleted before this returns.
[[nodiscard]] HttpResponse http_request(const std::string& curl,
                                        const std::string& method,
                                        const std::string& url,
                                        const std::string& body,
                                        const std::filesystem::path& tmp_dir,
                                        unsigned timeout_s);

/// Deletes the request files http_request left in `tmp_dir` when its process
/// was killed mid-request: only names of the exact form http_request writes,
/// <kind>-<pid>-<seq>-<8 hex digits>.<ext> with kind req or resp (.json), hdr
/// or err (.txt), whose process is gone. Returns the number deleted.
std::size_t remove_stale_request_files(const std::filesystem::path& tmp_dir);

/// The file in an audit directory that holds an outcome the server has not
/// taken yet (the worker sends it again at its next start).
inline constexpr const char* kUnreportedAudit = "unreported.json";

/// Removes the directories in `audits_dir` (<state dir>/audits) that an earlier
/// session left: an audit that never finished, or one that finished clean and
/// was not removed yet. Only directories named by a unit id are touched. One
/// whose audit.json reports failures_total > 0 stays, as the record of a failed
/// audit, and so does one that holds kUnreportedAudit. Returns how many it
/// removed.
std::size_t remove_stale_audit_dirs(const std::filesystem::path& audits_dir);

/// Status code of the last "HTTP/x NNN" line of a curl -D header dump, 0 if none.
[[nodiscard]] int parse_http_status(std::string_view headers) noexcept;

/// True for https:// URLs, and for http:// only to localhost, 127.0.0.1 or
/// [::1]: the worker token travels in the request body.
[[nodiscard]] bool server_url_ok(std::string_view url) noexcept;

} // namespace collatz::worker

#endif // COLLATZ_WORKER_HTTP_HPP
