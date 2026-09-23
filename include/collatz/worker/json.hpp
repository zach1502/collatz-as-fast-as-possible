#ifndef COLLATZ_WORKER_JSON_HPP
#define COLLATZ_WORKER_JSON_HPP

// JSON text helpers of `collatz worker`, the worker token kept out of printed
// text, server text made printable, and the result-digest text rebuilt from a
// `verify --json` document.

#include "collatz/checkpoint.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace collatz::worker {

/// JSON string literal for `s` (control characters as \u00XX).
[[nodiscard]] std::string json_quote(std::string_view s);

/// Registers the worker token: from then on redact() and printable() replace
/// it, both as is and as json_quote writes it inside a JSON string, with
/// "[token]". The worker calls it once, before it starts any thread; an empty
/// `secret` clears it.
void set_secret(std::string_view secret);

/// `s` with the registered secret replaced by "[token]" and the value of every
/// "Authorization" header or field (after "Authorization:" or
/// "Authorization=", up to the end of the line or a quote) by "[redacted]".
/// The worker passes every line it logs and all server and curl text through
/// it, so that a server reply that echoes the token does not print it.
[[nodiscard]] std::string redact(std::string_view s);

/// Server text made safe for a terminal: redact()ed, every byte outside
/// printable ASCII becomes '?', and text past `max_len` bytes is cut and ends
/// in "...".
[[nodiscard]] std::string printable(std::string_view s, std::size_t max_len = 400);

/// Compact JSON text of a parsed value; numbers keep their source text, so
/// u64 counters round-trip exactly.
[[nodiscard]] std::string json_dump(const checkpoint::Json& v);

/// Text of string member `key` of `obj`; "" when `obj` is null, has no such
/// member, or the member is not a string.
[[nodiscard]] std::string string_member(const checkpoint::Json* obj, const char* key);

/// The canonical text behind result.result_digest, rebuilt from the fields of
/// a `verify --json` document: equal to checkpoint::result_digest_text for the
/// run that wrote the document, and to the text the server hashes
/// (server/src/result/digest.ts digestText). False with a reason on a missing or
/// malformed field, or when result.result_digest_schema is not
/// checkpoint::kResultDigestSchema.
[[nodiscard]] bool
digest_text_from_json(const checkpoint::Json& doc, std::string& text, std::string& err);

} // namespace collatz::worker

#endif // COLLATZ_WORKER_JSON_HPP
