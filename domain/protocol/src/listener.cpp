#include <core/hash/base64.h>
#include <core/json/json.h>
#include <core/platform/random.h>
#include <domain/protocol/listener.h>
#include <foundation/io/vfs.h>

namespace engine::protocol {

namespace {

// Compares without stopping at the first difference, so how long a refusal takes says nothing
// about how much of a guessed token was right. Loopback makes the timing hard to read anyway; it
// costs nothing to not offer it.
bool same_secret(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  u8 diff = 0;
  for (usize i = 0; i < a.size(); ++i)
    diff = static_cast<u8>(diff | (static_cast<u8>(a[i]) ^ static_cast<u8>(b[i])));
  return diff == 0;
}

}  // namespace

bool Listener::start(u16 port, std::string shell_dir, std::string session, std::string& error) {
  u8 secret[16] = {};
  if (!platform::secure_random(std::span<u8>(secret, sizeof(secret)))) {
    error = "the system's secure random source did not answer, so there is no token to give";
    return false;
  }
  token_ = base64_encode(std::span<const u8>(secret, sizeof(secret)), Base64Alphabet::Url, false);
  shell_dir_ = std::move(shell_dir);
  session_ = std::move(session);
  return server_.start(port, *this, error);
}

std::string Listener::shell_url() const {
  std::string shell = "http://127.0.0.1:" + std::to_string(port()) + "/?token=" + token_;
  if (!session_.empty()) shell += "&session=" + session_;
  return shell;
}

std::string Listener::announcement() const {
  JsonValue line = JsonValue::object();
  line.set("listen", "ws://127.0.0.1:" + std::to_string(port()) + "/");
  line.set("token", token_);
  if (!session_.empty()) line.set("session", session_);
  line.set("shell", shell_url());
  JsonWriteOptions compact;
  compact.pretty = false;  // one line: it is the first line of stdout, which clients read as one
  return write_json(line, compact);
}

bool Listener::host_allowed(const net::HttpRequest& request) const {
  // A page served from elsewhere that rebinds its own name to 127.0.0.1 still sends its own name
  // here; only the listener's own address passes.
  const std::string* host = request.header("host");
  if (host == nullptr) return false;
  const std::string port_text = ":" + std::to_string(port());
  return *host == "127.0.0.1" + port_text || *host == "localhost" + port_text;
}

bool Listener::token_given(const net::HttpRequest& request) const {
  std::string given;
  if (const std::string* header = request.header("x-engine-token"); header != nullptr) {
    given = *header;
  } else if (!request.query_param("token", given)) {
    return false;
  }
  return same_secret(given, token_);
}

void Listener::http(const net::HttpRequest& request, net::HttpResponse& response) {
  if (!host_allowed(request)) {
    response = net::http_error(403, "the Host header must be this listener's own address");
    return;
  }
  if (request.method != "GET") {
    response = net::http_error(405, "this listener answers GET and the WebSocket upgrade only");
    return;
  }
  const std::string_view path = request.path;
  if (path == "/" || path == "/index.html") {
    serve_static("index.html", response);
  } else if (path.starts_with("/shell/")) {
    serve_static(path.substr(7), response);
  } else if (path.starts_with("/capture/")) {
    if (!token_given(request)) {
      response = net::http_error(403, "a capture needs the session token");
      return;
    }
    serve_capture(path.substr(9), response);
  } else {
    response = net::http_error(404);
  }
}

void Listener::serve_static(std::string_view relative, net::HttpResponse& response) const {
  if (shell_dir_.empty()) {
    response = net::http_error(404, "this host serves no shell (no shell directory)");
    return;
  }
  if (!net::safe_relative_path(relative)) {
    response = net::http_error(404);
    return;
  }
  std::string bytes;
  const std::string file = shell_dir_ + "/" + std::string(relative);
  if (io::read_file(file, bytes) != io::Status::Ok) {
    response = net::http_error(404);
    return;
  }
  response.status = 200;
  response.content_type = std::string(net::content_type_for(relative));
  response.body = std::move(bytes);
}

void Listener::serve_capture(std::string_view rest, net::HttpResponse& response) {
  // "/capture/<id>" is the colour PNG; "/capture/<id>/<part>" another channel (engine-host: "ids").
  const usize slash = rest.find('/');
  const std::string_view id = rest.substr(0, slash);
  const std::string_view part =
      slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
  if (captures_ == nullptr) {
    response = net::http_error(404, "this host keeps no captures");
    return;
  }
  // Under the lock, so the source reads the host's state as a method would.
  std::lock_guard<std::mutex> lock(*lock_);
  response = net::http_error(404);
  if (!captures_->capture(id, part, response) && response.status == 200) {
    response = net::http_error(404);
  }
}

bool Listener::accept(const net::HttpRequest& request, u16& close, std::string& reason) {
  close = net::close_code::k_policy;
  if (!host_allowed(request)) {
    reason = "the Host header must be this listener's own address";
    return false;
  }
  // A browser always sends Origin on a WebSocket; a page from anywhere but this listener is
  // refused even when it somehow has the token. Clients that are not browsers send none.
  if (const std::string* origin = request.header("origin"); origin != nullptr) {
    const std::string port_text = ":" + std::to_string(port());
    if (*origin != "http://127.0.0.1" + port_text && *origin != "http://localhost" + port_text) {
      reason = "a browser page may connect only from this listener's own origin";
      return false;
    }
  }
  if (request.path != "/") {
    reason = "the protocol is served at /";
    return false;
  }
  std::string given;
  if (request.header("x-engine-token") == nullptr && !request.query_param("token", given)) {
    reason = "a session token is required: ?token=<the token the host printed>";
    return false;
  }
  if (!token_given(request)) {
    reason = "the token does not match this host's";
    return false;
  }
  return true;
}

std::string Listener::message(u64, std::string_view text) {
  // The one lock the host's own loop takes too: a request over the socket and a line on stdin (or
  // the editor's frame) go through the same dispatcher, one at a time, with the same attribution,
  // roles and journal.
  std::lock_guard<std::mutex> lock(*lock_);
  return dispatcher_->dispatch_text(text);
}

}  // namespace engine::protocol
