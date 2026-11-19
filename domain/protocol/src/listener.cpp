#include <core/hash/base64.h>
#include <core/hash/sha256.h>
#include <core/json/json.h>
#include <core/platform/random.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/protocol/listener.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>

#include <chrono>

namespace engine::protocol {

namespace {

// The cookie a spent ticket buys a browser page (ADR-0069), named for the listener's port: a
// browser keeps one cookie per name, host and path and none per port (RFC 6265 §8.5), so two hosts'
// shells under one name would overwrite each other's and break the first one's captures and
// reconnects. The port in the name does not keep the cookie from another port's server: the
// browser still sends every one of them to every port of 127.0.0.1 (the ADR's "What this is not").
std::string cookie_name(u16 port) { return "engine_session_" + std::to_string(port); }

// A query pair that may go back into a header as it came: printable ASCII, no space.
bool plain_query(std::string_view pair) noexcept {
  for (const char c : pair) {
    if (c <= ' ' || c > '~') return false;
  }
  return true;
}

// The value of cookie `name` in a Cookie header, or false.
bool cookie_value(const net::HttpRequest& request, std::string_view name, std::string& out) {
  const std::string* header = request.header("cookie");
  if (header == nullptr) return false;
  std::string_view rest = *header;
  while (!rest.empty()) {
    const usize semi = rest.find(';');
    std::string_view pair = rest.substr(0, semi);
    rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
    while (!pair.empty() && pair.front() == ' ')
      pair.remove_prefix(1);
    const usize eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
      out.assign(pair.substr(eq + 1));
      return true;
    }
  }
  return false;
}

}  // namespace

// ---- credentials (clients.h) ------------------------------------------------------------------

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

bool make_token(std::string& out) {
  u8 secret[16] = {};
  if (!platform::secure_random(std::span<u8>(secret, sizeof(secret)))) return false;
  out = base64_encode(std::span<const u8>(secret, sizeof(secret)), Base64Alphabet::Url, false);
  return true;
}

std::string token_digest(std::string_view token) { return sha256_hex(sha256(token)); }

bool Credentials::load(std::string_view path, std::string_view text, const Policy* policy,
                       std::string& error) {
  JsonValue value;
  if (!parse_json(text, value).ok) {
    error = std::string(path) + " is not JSON";
    return false;
  }
  CredentialsFile file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, value, ctx)) {
    error = std::string(path) + " is not a credentials file" +
            (ctx.diagnostics.empty()
                 ? std::string()
                 : ": " + ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message);
    return false;
  }
  for (u32 i = 0; i < file.credentials.size(); ++i) {
    const Credential& c = file.credentials[i];
    const std::string where = std::string(path) + ": credential " + std::to_string(i);
    if (c.name.empty() || c.actor.empty()) {
      error = where + " needs a name and an actor: a connection is somebody";
      return false;
    }
    for (u32 j = 0; j < i; ++j) {
      if (file.credentials[j].name == c.name) {
        error = where + " repeats the name '" + c.name + "'";
        return false;
      }
    }
    bool hex = c.token_sha256.size() == 64;
    for (const char ch : c.token_sha256)
      hex = hex && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    if (!hex) {
      error = where + " ('" + c.name + "') has no token_sha256 of 64 lowercase hex digits";
      return false;
    }
    if (!c.role.empty() && policy != nullptr && policy->roles_loaded() &&
        policy->find_role(c.role) == nullptr) {
      error = where + " ('" + c.name + "') names role '" + c.role +
              "', which the host's roles file does not have";
      return false;
    }
  }
  file_ = std::move(file);
  return true;
}

u32 Credentials::read_only_count(const Policy* policy) const noexcept {
  u32 n = 0;
  for (const Credential& c : file_.credentials)
    n += &bound_role(policy, c.role) == &bound_role(nullptr, {}) ? 1u : 0u;
  return n;
}

const Credential* Credentials::match(std::string_view token) const {
  if (token.empty()) return nullptr;
  const std::string digest = token_digest(token);
  const Credential* found = nullptr;
  // Every entry is compared, so the time does not say which one matched.
  for (const Credential& c : file_.credentials) {
    if (same_secret(digest, c.token_sha256) && found == nullptr) found = &c;
  }
  return found;
}

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
  stop_looker();
  events_.attach(nullptr);
  server_.stop();
}

// ---- the ledger's looker (protocol.md, "The ledger's events") ---------------------------------

void Listener::wake_looker(void* self) {
  Listener* l = static_cast<Listener*>(self);
  {
    std::lock_guard<std::mutex> lock(l->look_mutex_);
    l->look_wanted_ = true;
  }
  l->look_cv_.notify_one();
}

void Listener::start_looker() {
  SessionManager* sessions = dispatcher_->context().sessions;
  if (sessions == nullptr || looker_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(look_mutex_);
    look_stop_ = false;
    look_wanted_ = false;
  }
  looker_ = std::thread([this] { look_loop(); });
  sessions->set_ledger_waker(&Listener::wake_looker, this);
}

void Listener::stop_looker() {
  // The waker goes first, so no watch wakes a looker that is gone.
  if (SessionManager* sessions = dispatcher_->context().sessions; sessions != nullptr)
    sessions->set_ledger_waker(nullptr, nullptr);
  {
    std::lock_guard<std::mutex> lock(look_mutex_);
    look_stop_ = true;
  }
  look_cv_.notify_all();
  if (looker_.joinable()) looker_.join();
}

void Listener::look_loop() {
  std::unique_lock<std::mutex> wait(look_mutex_);
  Vector<SessionManager::LedgerLook> looks;
  for (;;) {
    // No timeout: the looker wakes for a notice and for nothing else.
    look_cv_.wait(wait, [this] { return look_wanted_ || look_stop_; });
    if (look_stop_) return;
    // The rest of a burst lands in this quarter second and is the same look.
    if (look_cv_.wait_for(wait, std::chrono::milliseconds(k_ledger_look_ms),
                          [this] { return look_stop_; }))
      return;
    look_wanted_ = false;
    wait.unlock();
    bool again = false;
    {
      SessionManager* sessions = dispatcher_->context().sessions;
      std::unique_lock<std::mutex> host(*lock_);
      if (sessions != nullptr) {
        sessions->begin_ledger_looks(false, looks);
        // The read, the hash and the parse are the look's cost and touch nothing of the host's,
        // so the host's clients are answered beside them; the lock is taken back to publish.
        host.unlock();
        for (SessionManager::LedgerLook& look : looks)
          SessionManager::read_ledger_look(sessions->vfs(), look);
        host.lock();
        (void)sessions->finish_ledger_looks(looks, again);
      }
    }
    wait.lock();
    if (again) look_wanted_ = true;
  }
}

bool Listener::start(u16 port, std::string shell_dir, std::string session, std::string& error) {
  u8 secret[16] = {};
  if (!platform::secure_random(std::span<u8>(secret, sizeof(secret)))) {
    error = "the system's secure random source did not answer, so there is no token to give";
    return false;
  }
  token_ = base64_encode(std::span<const u8>(secret, sizeof(secret)), Base64Alphabet::Url, false);
  shell_dir_ = std::move(shell_dir);
  shell_roots_ = io::Roots{};
  // A shell directory that is not there serves nothing (every GET a 404 that says so), as before.
  if (!shell_dir_.empty() && shell_roots_.add("shell", shell_dir_, false) != io::Status::Ok) {
    shell_dir_.clear();
  }
  session_ = std::move(session);
  events_.attach(this);
  if (server_.start(port, *this, error)) {
    start_looker();
    return true;
  }
  events_.attach(nullptr);
  return false;
}

std::string Listener::shell_url() {
  // A ticket, not the token: the address is what browser history, a screenshot and an error keep,
  // and a ticket is worth nothing once its page has loaded (ADR-0069).
  std::string ticket;
  if (!make_token(ticket)) return {};
  {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    const i64 now = time::wall_unix_ms();
    // Expired tickets go as new ones come, so the list holds only the live ones.
    for (u32 i = tickets_.size(); i > 0; --i) {
      if (now >= tickets_[i - 1].expires_unix_ms) tickets_.erase_at(i - 1);
    }
    tickets_.push_back({ticket, now + ticket_lifetime_ms_});
  }
  std::string shell = "http://127.0.0.1:" + std::to_string(port()) + "/?ticket=" + ticket;
  if (!session_.empty()) shell += "&session=" + session_;
  return shell;
}

bool Listener::spend_ticket(std::string_view ticket, std::string& cookie) {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  const i64 now = time::wall_unix_ms();
  for (u32 i = 0; i < tickets_.size(); ++i) {
    if (!same_secret(tickets_[i].value, ticket)) continue;
    // Spent or expired, it is gone either way: a ticket buys one cookie, within its lifetime.
    const bool live = now < tickets_[i].expires_unix_ms;
    tickets_.erase_at(i);
    if (!live || !make_token(cookie)) return false;
    cookies_.push_back(cookie);
    return true;
  }
  return false;
}

Listener::Presented Listener::presented(const net::HttpRequest& request) {
  Presented out;
  if (const std::string* header = request.header("x-engine-token"); header != nullptr) {
    if (same_secret(*header, token_)) {
      out.known = true;
    } else if (credentials_ != nullptr) {
      out.credential = credentials_->match(*header);
      out.known = out.credential != nullptr;
    }
    if (!out.known)
      out.why = "the X-Engine-Token header names no token or credential this host knows";
    return out;
  }
  std::string cookie;
  if (cookie_value(request, cookie_name(port()), cookie)) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    for (const std::string& c : cookies_)
      out.known = out.known || same_secret(c, cookie);
    if (!out.known)
      out.why = "the page's cookie is not one this host gave; open a new shell address";
    return out;
  }
  out.why =
      "a credential is required: the X-Engine-Token header (the token the host printed, or "
      "one the operator issued), or a page opened from the shell address the host printed";
  return out;
}

std::string Listener::announcement() {
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

void Listener::http(const net::HttpRequest& request, net::HttpResponse& response) {
  // Every answer, refusals and 404s included, since a 404's body is drawn by the browser too.
  answer_http(request, response);
  response.headers.push_back({"Content-Security-Policy", std::string(k_shell_content_policy)});
}

void Listener::answer_http(const net::HttpRequest& request, net::HttpResponse& response) {
  if (!host_allowed(request)) {
    response = net::http_error(403, "the Host header must be this listener's own address");
    return;
  }
  if (request.method != "GET") {
    response = net::http_error(405, "this listener answers GET and the WebSocket upgrade only");
    return;
  }
  const std::string_view path = request.path;
  std::string ticket;
  if (path == "/" && request.query_param("ticket", ticket)) {
    // The shell's address: its one-time ticket buys the page a cookie, and the page is sent on to
    // an address with no ticket in it, so what history keeps is worth nothing (ADR-0069).
    std::string cookie;
    // A browser that already holds this host's cookie (the printed address opened a second time)
    // is sent on without a new one.
    const bool spent = spend_ticket(ticket, cookie);
    if (!spent && !presented(request).known) {
      response = net::http_error(403,
                                 "this shell address was used already, or is not this host's: "
                                 "the host makes a new one for each page it opens");
      return;
    }
    // The rest of the query goes on as it came (still percent-encoded, so it cannot break the
    // header): the session, and engine-editor's `embedded=1`.
    std::string rest;
    std::string_view query = request.query;
    while (!query.empty()) {
      const usize amp = query.find('&');
      const std::string_view pair = query.substr(0, amp);
      query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
      if (pair.starts_with("ticket=") || pair.empty() || !plain_query(pair)) continue;
      rest += (rest.empty() ? "?" : "&") + std::string(pair);
    }
    response.status = 303;
    response.headers.push_back({"Location", "/" + rest});
    if (spent)
      response.headers.push_back({"Set-Cookie", cookie_name(port()) + "=" + cookie +
                                                    "; HttpOnly; SameSite=Strict; Path=/"});
    return;
  }
  if (path == "/" || path == "/index.html") {
    serve_static("index.html", response);
  } else if (path.starts_with("/shell/")) {
    serve_static(path.substr(7), response);
  } else if (path.starts_with("/capture/")) {
    // A credential's holder sees what the operator's do: a read is never restricted.
    const Presented who = presented(request);
    if (!who.known) {
      response = net::http_error(403, "a capture needs a credential: " + who.why);
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
  // The lexical check first (no leading '/', no '\', ':', '..'), then the roots' rules against the
  // shell directory alone: a component ending in a dot or a space ("..%20" decodes to ".. "), a
  // device name (`CON` would be opened as the console by this thread), a link that leads out.
  // Every refusal is the same 404 as a file that is not there.
  io::Admitted file;
  io::Refusal why = io::Refusal::none;
  if (!net::safe_relative_path(relative) ||
      !shell_roots_.admit(relative, shell_roots_.roots()[0].canonical, io::Access::read, file,
                          why)) {
    response = net::http_error(404);
    return;
  }
  std::string bytes;
  if (io::read_file(file.native, bytes) != io::Status::Ok) {
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

bool Listener::accept(u64 connection, const net::HttpRequest& request, u16& close,
                      std::string& reason) {
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
  // Who it is, once, here, before its first message is read: nothing runs for a connection that
  // presented nothing the host knows (ADR-0069). A token in the URL is not read at all.
  const Presented who = presented(request);
  if (!who.known) {
    reason = who.why;
    return false;
  }
  // Bound under the dispatch lock, which every request takes, so the first message already finds
  // its identity.
  std::lock_guard<std::mutex> lock(*lock_);
  Client& client = clients_.touch(connection, time::wall_unix_ms());
  if (who.credential != nullptr) {
    client.credential = who.credential->name;
    client.who = doc::Attribution{};
    client.who.actor = who.credential->actor;
    client.who.role = who.credential->role;
    client.who.task = who.credential->task;
    client.identified = true;
    client.bound = true;
  } else {
    client.credential = "operator";
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
  // A handler may let the lock go for work that reads nothing of the host's (rpc.h,
  // `without_dispatch_lock`), so other connections are served meanwhile; it takes it back first.
  ctx.dispatch_lock = lock_;
  std::string reply = dispatcher_->dispatch_text(text);
  ctx.connection = 0;
  ctx.dispatch_lock = nullptr;
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
