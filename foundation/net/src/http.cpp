#include <foundation/net/http.h>
#include <foundation/net/socket.h>

namespace engine::net {

namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
  if (s.size() < suffix.size()) return false;
  const std::string_view tail = s.substr(s.size() - suffix.size());
  for (usize i = 0; i < suffix.size(); ++i) {
    if (lower(tail[i]) != suffix[i]) return false;
  }
  return true;
}

}  // namespace

const std::string* HttpRequest::header(std::string_view lowercase_name) const noexcept {
  for (const auto& entry : headers) {
    if (entry.first == lowercase_name) return &entry.second;
  }
  return nullptr;
}

bool HttpRequest::query_param(std::string_view name, std::string& out) const {
  std::string_view rest = query;
  while (!rest.empty()) {
    const usize amp = rest.find('&');
    const std::string_view pair = rest.substr(0, amp);
    rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
    const usize eq = pair.find('=');
    std::string key;
    if (!percent_decode(pair.substr(0, eq), key, true) || key != name) continue;
    const std::string_view value =
        eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
    return percent_decode(value, out, true);
  }
  return false;
}

bool percent_decode(std::string_view text, std::string& out, bool plus_is_space) {
  out.clear();
  out.reserve(text.size());
  for (usize i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      if (i + 2 >= text.size()) return false;
      const int hi = hex_value(text[i + 1]);
      const int lo = hex_value(text[i + 2]);
      if (hi < 0 || lo < 0) return false;
      const char decoded = static_cast<char>(hi * 16 + lo);
      if (decoded == '\0') return false;
      out.push_back(decoded);
      i += 2;
    } else if (c == '+' && plus_is_space) {
      out.push_back(' ');
    } else if (c == '\0') {
      return false;
    } else {
      out.push_back(c);
    }
  }
  return true;
}

bool parse_http_request(std::string_view head, HttpRequest& out) {
  out = HttpRequest{};
  for (usize i = 0; i < head.size(); ++i) {
    if (head[i] == '\0') return false;
    if (head[i] == '\r' && (i + 1 >= head.size() || head[i + 1] != '\n')) return false;
  }
  usize line_end = head.find("\r\n");
  const std::string_view request_line = head.substr(0, line_end);
  const usize sp1 = request_line.find(' ');
  const usize sp2 = sp1 == std::string_view::npos ? sp1 : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
      request_line.find(' ', sp2 + 1) != std::string_view::npos) {
    return false;
  }
  out.method = std::string(request_line.substr(0, sp1));
  out.target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
  out.version = std::string(request_line.substr(sp2 + 1));
  if (out.method.empty() || out.target.empty() || out.target[0] != '/') return false;
  if (!out.version.starts_with("HTTP/1.")) return false;
  const usize question = out.target.find('?');
  const std::string_view raw_path = std::string_view(out.target).substr(0, question);
  if (question != std::string::npos) out.query = out.target.substr(question + 1);
  if (!percent_decode(raw_path, out.path)) return false;

  while (line_end != std::string_view::npos) {
    const usize start = line_end + 2;
    if (start >= head.size()) break;
    line_end = head.find("\r\n", start);
    const std::string_view line = head.substr(
        start, line_end == std::string_view::npos ? std::string_view::npos : line_end - start);
    if (line.empty()) break;
    // Obsolete line folding (a header continued on a line starting with a blank) is refused, as
    // RFC 9112 §5.2 allows a server to.
    if (line.front() == ' ' || line.front() == '\t') return false;
    const usize colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    std::string name(line.substr(0, colon));
    for (char& c : name)
      c = lower(c);
    out.headers.push_back({std::move(name), std::string(trim(line.substr(colon + 1)))});
    if (out.headers.size() > 100) return false;
  }
  return true;
}

HttpRead read_http_request(Socket& socket, i32 timeout_ms, HttpRequest& out,
                           std::string& leftover) {
  std::string buffer = std::move(leftover);
  leftover.clear();
  u8 chunk[4096];
  for (;;) {
    const usize end = buffer.find("\r\n\r\n");
    if (end != std::string::npos) {
      if (end + 2 > k_max_http_head) return HttpRead::TooLarge;
      leftover = buffer.substr(end + 4);
      return parse_http_request(std::string_view(buffer).substr(0, end + 2), out)
                 ? HttpRead::Ok
                 : HttpRead::Malformed;
    }
    if (buffer.size() > k_max_http_head) return HttpRead::TooLarge;
    const i32 ready = socket.wait_readable(timeout_ms);
    if (ready == 0) return HttpRead::TimedOut;
    if (ready < 0) return HttpRead::Closed;
    const i64 n = socket.receive(std::span<u8>(chunk, sizeof(chunk)));
    if (n <= 0) return HttpRead::Closed;
    buffer.append(reinterpret_cast<const char*>(chunk), static_cast<usize>(n));
  }
}

std::string_view http_status_text(u32 status) noexcept {
  switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 426: return "Upgrade Required";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

std::string format_http_response(const HttpResponse& response) {
  std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                    std::string(http_status_text(response.status)) + "\r\n";
  if (!response.content_type.empty()) out += "Content-Type: " + response.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
  for (const auto& h : response.headers)
    out += h.first + ": " + h.second + "\r\n";
  out += "\r\n";
  out += response.body;
  return out;
}

HttpResponse http_error(u32 status, std::string_view detail) {
  HttpResponse r;
  r.status = status;
  r.content_type = "text/plain; charset=utf-8";
  r.body = std::string(http_status_text(status));
  if (!detail.empty()) r.body += ": " + std::string(detail);
  r.body += "\n";
  return r;
}

std::string_view content_type_for(std::string_view path) noexcept {
  if (ends_with_ci(path, ".html") || ends_with_ci(path, ".htm")) return "text/html; charset=utf-8";
  if (ends_with_ci(path, ".css")) return "text/css; charset=utf-8";
  if (ends_with_ci(path, ".js") || ends_with_ci(path, ".mjs"))
    return "text/javascript; charset=utf-8";
  if (ends_with_ci(path, ".json")) return "application/json";
  if (ends_with_ci(path, ".png")) return "image/png";
  if (ends_with_ci(path, ".svg")) return "image/svg+xml";
  if (ends_with_ci(path, ".ico")) return "image/x-icon";
  if (ends_with_ci(path, ".woff2")) return "font/woff2";
  if (ends_with_ci(path, ".txt")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

bool safe_relative_path(std::string_view relative) noexcept {
  if (relative.empty() || relative.front() == '/') return false;
  for (const char c : relative) {
    if (c == '\\' || c == ':' || c == '\0') return false;
  }
  std::string_view rest = relative;
  while (true) {
    const usize slash = rest.find('/');
    const std::string_view segment = rest.substr(0, slash);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (slash == std::string_view::npos) break;
    rest = rest.substr(slash + 1);
  }
  return true;
}

}  // namespace engine::net
