#include <core/hash/base64.h>
#include <core/json/json.h>
#include <core/platform/random.h>
#include <core/time/time.h>
#include <domain/protocol/listener.h>
#include <domain/protocol/session.h>
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

// ---- Clients (clients.h) ----------------------------------------------------------------------
// Here rather than in a file of its own: the listener is the only thing that makes or forgets one.

Client& Clients::touch(u64 connection, i64 now_ms) {
  // A host has tens of connections at most, so a walk is the right cost; they are numbered as they
  // are accepted, and inserting by number keeps `client.list` in that order.
  for (Client& c : clients_) {
    if (c.connection == connection) return c;
  }
  Client client;
  client.connection = connection;
  client.connected_unix_ms = now_ms;
  u32 at = clients_.size();
  while (at > 0 && clients_[at - 1].connection > connection)
    --at;
  clients_.insert(at, std::move(client));
  return clients_[at];
}

Client* Clients::find(u64 connection) noexcept {
  for (Client& c : clients_) {
    if (c.connection == connection) return &c;
  }
  return nullptr;
}

const Client* Clients::find(u64 connection) const noexcept {
  for (const Client& c : clients_) {
    if (c.connection == connection) return &c;
  }
  return nullptr;
}

void Clients::forget(u64 connection) {
  for (u32 i = 0; i < clients_.size(); ++i) {
    if (clients_[i].connection == connection) {
      clients_.erase_at(i);
      return;
    }
  }
}

// ---- Listener ---------------------------------------------------------------------------------

Listener::Listener(Dispatcher& dispatcher, std::mutex& dispatch_lock, CaptureSource* captures)
    : dispatcher_(&dispatcher), lock_(&dispatch_lock), captures_(captures) {
  Context& ctx = dispatcher.context();
  ctx.events = &events_;
  ctx.clients = &clients_;
  if (ctx.sessions != nullptr) ctx.sessions->set_events(&events_);
}

Listener::~Listener() {
  stop();
  Context& ctx = dispatcher_->context();
  if (ctx.clients == &clients_) ctx.clients = nullptr;
  if (ctx.events == &events_) ctx.events = nullptr;
  if (ctx.sessions != nullptr && ctx.sessions->events() == &events_) {
    ctx.sessions->set_events(nullptr);
  }
}

void Listener::stop() {
  events_.attach(nullptr);
  server_.stop();
}

bool Listener::start(u16 port, std::string shell_dir, std::string session, std::string& error) {
  u8 secret[16] = {};
  if (!platform::secure_random(std::span<u8>(secret, sizeof(secret)))) {
    error = "the system's secure random source did not answer, so there is no token to give";
    return false;
  }
  token_ = base64_encode(std::span<const u8>(secret, sizeof(secret)), Base64Alphabet::Url, false);
  shell_dir_ = std::move(shell_dir);
  session_ = std::move(session);
  events_.attach(this);
  if (server_.start(port, *this, error)) return true;
  events_.attach(nullptr);
  return false;
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

std::string Listener::message(u64 connection, std::string_view text) {
  // The one lock the host's own loop takes too: a request over the socket and a line on stdin (or
  // the editor's frame) go through the same dispatcher, one at a time, with the same attribution,
  // roles and journal.
  std::lock_guard<std::mutex> lock(*lock_);
  // Which connection this request came on — for `session.subscribe`, for the caller its
  // `client.hello` named, for the sessions and leases it holds — and only for this request.
  ++clients_.touch(connection, time::wall_unix_ms()).requests;
  Context& ctx = dispatcher_->context();
  ctx.connection = connection;
  std::string reply = dispatcher_->dispatch_text(text);
  ctx.connection = 0;
  return reply;
}

void Listener::closed(u64 connection) {
  // A client that vanishes gives back what it held, under the same lock as every request, so a
  // lease is released between two other clients' calls and never during one: its leases first
  // (each `event.session` "lease_released" with the reason), then its holds on sessions (a session
  // nobody else holds closes, "closed"), then its subscriptions and its entry.
  {
    std::lock_guard<std::mutex> lock(*lock_);
    if (SessionManager* sessions = dispatcher_->context().sessions) {
      sessions->release_connection(connection, time::wall_unix_ms());
    }
    clients_.forget(connection);
  }
  events_.forget(connection);
}

EventTransport::Post Listener::post(u64 connection, std::string text, u32 bound) {
  switch (server_.post(connection, std::move(text), bound)) {
    case net::Post::Queued: return Post::Queued;
    case net::Post::Full: return Post::Full;
    case net::Post::Gone: break;
  }
  return Post::Gone;
}

}  // namespace engine::protocol
