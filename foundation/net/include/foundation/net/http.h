#pragma once

// The HTTP/1.1 subset a loopback listener needs (docs/subsystems/net.md): one request head read
// and parsed, a GET answered with a body, and the WebSocket upgrade (websocket.h). No request
// bodies, no chunked encoding, no keep-alive (every answer says `Connection: close`), no TLS, no
// proxy forms of the target. A browser's page load is a handful of GETs on fresh connections,
// which loopback makes free, and a parser this small is one a reader can check line by line.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <string>
#include <string_view>
#include <utility>

namespace engine::net {

class Socket;

struct HttpRequest {
  std::string method;   // "GET"
  std::string target;   // as sent: "/shell/main.js?token=..."
  std::string path;     // the target before '?', percent-decoded: "/shell/main.js"
  std::string query;    // after '?', still encoded
  std::string version;  // "HTTP/1.1"
  // Header names lowercased, values with the surrounding blanks trimmed, in the order sent.
  Vector<std::pair<std::string, std::string>> headers;

  // The first header of that (lowercase) name, or null.
  const std::string* header(std::string_view lowercase_name) const noexcept;
  // A query parameter's value, percent-decoded; false when it is absent.
  bool query_param(std::string_view name, std::string& out) const;
};

// Parses a request head (the request line and headers, up to but not including the blank line).
// False for anything malformed: a request line without three parts, a header line without a colon,
// a target that is not origin-form ('/...') or does not decode, a NUL or a bare CR anywhere.
bool parse_http_request(std::string_view head, HttpRequest& out);

enum class HttpRead : u8 { Ok, Closed, TooLarge, Malformed, TimedOut };

// The largest request head accepted, request line and headers together. A browser's is under 2 KB.
inline constexpr usize k_max_http_head = 16 * 1024;

// Reads one request head from `socket`, waiting at most `timeout_ms` for each piece. Bytes that
// arrived after the head (a WebSocket client may send its first frame at once) are left in
// `leftover`.
HttpRead read_http_request(Socket& socket, i32 timeout_ms, HttpRequest& out, std::string& leftover);

struct HttpResponse {
  u32 status = 200;
  std::string content_type;  // empty: no body type sent
  std::string body;
  Vector<std::pair<std::string, std::string>> headers;  // extra headers
};

// "HTTP/1.1 <status> <reason>", the headers, Content-Length, Connection: close, the body. A
// response always says `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`: the shell
// is edited while it is served, and a capture's bytes change under the same id across hosts.
std::string format_http_response(const HttpResponse& response);
std::string_view http_status_text(u32 status) noexcept;
// A response whose body is the status line's reason in plain text.
HttpResponse http_error(u32 status, std::string_view detail = {});

// %XX and nothing else ('+' is a plus in a path; the query's '+' is decoded as a space by
// `query_param`). False for a truncated or non-hex escape, or one that decodes to NUL.
bool percent_decode(std::string_view text, std::string& out, bool plus_is_space = false);

// The content type of a static file by its extension: html, css, js/mjs, json, png, svg, ico,
// woff2, txt; anything else is application/octet-stream.
std::string_view content_type_for(std::string_view path) noexcept;

// Whether a decoded path relative to a served directory stays inside it: not empty, no segment
// that is "." or "..", no backslash, no ':' (a drive or a stream), no NUL, no empty segment, and
// no leading '/'. What a static file server checks before it joins the path to its root.
bool safe_relative_path(std::string_view relative) noexcept;

}  // namespace engine::net
