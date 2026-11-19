#pragma once

// The protocol over a loopback WebSocket (docs/subsystems/protocol.md, "Transport"): a
// `Dispatcher` served by foundation/net, which can bind 127.0.0.1 and nothing else, plus the few
// HTTP GETs a browser shell needs — its static files, and whatever captures the host keeps. It is
// the protocol's, not one host's: engine-host serves it beside its stdio loop (`--listen`), and
// engine-editor runs one in process over the session it already holds, so the web shell beside
// its viewport is a client of the same host as everything else (roadmap A37).
//
// **Indistinguishable from stdin.** Every text message is one JSON-RPC request (or batch) handed to
// `Dispatcher::dispatch_text` under the lock the host's own loop takes, so attribution, roles, the
// read-only rule and the journal see a socket's request exactly as they see a line on stdin. The
// lock also keeps the host's state single-threaded: a second connection is *served* concurrently
// (its messages are read and answered without waiting for the first to close) but its requests
// run one at a time with everyone else's, and with whatever the host does under the same lock (the
// editor's frame).
//
// **The token.** Loopback stops the network, not the machine: any local process — and any web page
// the user's browser has open, since a browser lets a page open a WebSocket to 127.0.0.1 — can
// reach the port. So the listener makes a 128-bit token from the system's secure random source at
// start and refuses every upgrade that does not carry it — in an `X-Engine-Token` header, or for a
// browser page the cookie a one-time ticket in the shell's address bought, never in a URL — or a
// credential the operator issued (clients.h, ADR-0069), with close code 1008. An upgrade from a
// browser must also come from the listener's own origin, and every request must name the listener's
// own address in `Host`, which stops a page elsewhere from using DNS rebinding to reach the port.
// This is loopback and a shared secret, **not authentication of a user**: whoever holds the token
// is whoever the roles file says a call is, as on stdin (protocol.md, "Roles, leases and
// proposals").
//
// **Events.** The listener owns the host's subscriptions (events.h) and is their transport: it
// wires them into the dispatcher's context and its session manager when it is made, tells each
// dispatch which connection it came on (so `session.subscribe` knows its subscriber), posts each
// event to the subscriber's connection, where the server's writer for that connection sends it
// outside every lock (foundation/net/server.h, `Server::post`), and forgets a connection's
// subscriptions when it closes. A host that never starts the listener publishes nothing.
//
// **Clients.** The listener also owns the host's client table (clients.h): each connection's entry
// is made on its first request, `client.hello` names who it is, and when the connection ends the
// listener gives back, under the dispatch lock, the leases it took and the sessions it opened
// (SessionManager::release_connection) before it forgets it (protocol.md, "Several clients, one
// host").

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/protocol/clients.h>
#include <domain/protocol/events.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/roots.h>
#include <foundation/net/server.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace engine::protocol {

// The content security policy on every HTTP response the listener answers (engine_shell.md, "What
// another client's content cannot do"; roadmap A50): the shell's page, its scripts and stylesheet,
// a 404's body, the ticket's redirect and a capture alike, as a header, because a `<meta>` tag is
// read only by the page that carries it and only after the parser reaches it. The shell runs only
// its own files, loads images only from its listener (the slideshow's captures) or blobs it made,
// connects only to its listener (the socket and the capture fetches), and with Trusted Types and
// no policy a string handed to an HTML sink throws in the browser, whatever the lint
// (`shell-html-sink`, tools/lint.ps1) missed.
inline constexpr std::string_view k_shell_content_policy =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' blob:; "
    "connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'none'; require-trusted-types-for 'script'; trusted-types 'none'";

// What `GET /capture/<id>[/<part>]` serves: a host that keeps captures answers for them
// (engine-host keeps the last eight `render.capture {keep}` made). Called with the dispatch lock
// held, so it may read the host's state as a method would. False leaves `response` as the 404 to
// send (with a reason in its body when the source gives one); a listener with no source answers 404
// to all.
class CaptureSource {
 public:
  virtual ~CaptureSource() = default;
  virtual bool capture(std::string_view id, std::string_view part, net::HttpResponse& response) = 0;
};

class Listener final : public net::ServerHandler, public EventTransport {
 public:
  // `dispatch_lock` is the host's own: every request runs under it. `captures` may be null. The
  // dispatcher's context gets the listener's subscriptions (`events`), and so does its session
  // manager, for as long as the listener lives.
  Listener(Dispatcher& dispatcher, std::mutex& dispatch_lock, CaptureSource* captures = nullptr);
  ENGINE_NON_COPYABLE(Listener);
  ~Listener() override;

  // Makes the token and starts serving on 127.0.0.1:`port` (0: a free port). `shell_dir` is the
  // directory the shell's static files are served from (empty or missing: `GET /` is a 404 that
  // says so); `session` is the session the host opened at start, named in the shell's URL.
  bool start(u16 port, std::string shell_dir, std::string session, std::string& error);
  void stop();
  bool running() const noexcept { return server_.port() != 0; }
  // Every connection shut under its peer, the port and the token kept, so a page reconnects to
  // the same address: engine-editor's `--test-drop-socket-at` (net::Server::drop_connections).
  u32 drop_connections() { return server_.drop_connections(); }
  // One JSON-RPC notification to every connection, outside any request: engine-editor's
  // `editor.relayout` (net::Server::send_text_all). Not a subscription — every page hears it.
  u32 notify_all(std::string_view notification) { return server_.send_text_all(notification); }

  // The credentials the host accepts beside its own token (clients.h; `--credentials`). Set
  // before `start`; null accepts the host's token alone. Not owned.
  void set_credentials(const Credentials* credentials) noexcept { credentials_ = credentials; }
  // How long a shell address's ticket stays good unspent (ADR-0069): ten minutes unless a test
  // says otherwise. A page that spent one keeps its cookie for the host's life.
  static constexpr i64 k_ticket_lifetime_ms = 10 * 60 * 1000;
  void set_ticket_lifetime_ms(i64 ms) noexcept { ticket_lifetime_ms_ = ms; }

  u16 port() const noexcept { return server_.port(); }
  // The operator's token: printed once, presented in `X-Engine-Token`, never read from a URL.
  const std::string& token() const noexcept { return token_; }
  const std::string& session() const noexcept { return session_; }
  // The shell's address with a fresh one-time ticket and the session:
  // http://127.0.0.1:<port>/?ticket=...&session=... The first GET that names the ticket spends
  // it for the page's cookie (ADR-0069); each call makes another.
  std::string shell_url();
  // The one line a host prints on stdout before it serves:
  // {"listen":"ws://127.0.0.1:<port>/","token":"...","session":"...","shell":"http://..."}.
  std::string announcement();

  // The host's subscriptions: what engine-editor publishes its selection through, and what a
  // handler reaches as `Context::events`.
  Subscriptions& events() noexcept { return events_; }
  // The host's client connections (clients.h), what a handler reaches as `Context::clients`. Read
  // and written under the dispatch lock only.
  Clients& clients() noexcept { return clients_; }

  void http(const net::HttpRequest& request, net::HttpResponse& response) override;
  bool accept(u64 connection, const net::HttpRequest& request, u16& close,
              std::string& reason) override;
  std::string message(u64 connection, std::string_view text) override;
  void closed(u64 connection) override;
  Post post(u64 connection, std::string text, u32 bound) override;

 private:
  // Who a request is (ADR-0069): the operator, a credential, or nobody the host knows.
  struct Presented {
    bool known = false;
    const Credential* credential = nullptr;  // null for the operator
    // What the refusal says: which kind of credential was missing or wrong, never what was sent.
    std::string why;
  };
  bool host_allowed(const net::HttpRequest& request) const;
  // `http` without the policy header, which `http` adds to whatever this answered.
  void answer_http(const net::HttpRequest& request, net::HttpResponse& response);
  Presented presented(const net::HttpRequest& request);
  bool spend_ticket(std::string_view ticket, std::string& cookie);
  void serve_static(std::string_view relative, net::HttpResponse& response) const;
  void serve_capture(std::string_view rest, net::HttpResponse& response);

  // The ledger's looker (protocol.md, "The ledger's events"): a thread that sleeps until a
  // session's watch wakes it, waits `k_ledger_look_ms` for the rest of a burst, and looks at the
  // ledgers with the host's lock let go for the read, so a burst of another process's writes is
  // one look, and four looks a second at most, rather than a read under the lock per write.
  // Started by `start`, stopped by `stop` (never under the host's lock, which the looker takes).
  static constexpr i64 k_ledger_look_ms = 250;
  static void wake_looker(void* self);
  void start_looker();
  void stop_looker();
  void look_loop();
  std::thread looker_;
  std::mutex look_mutex_;
  std::condition_variable look_cv_;
  bool look_wanted_ = false;
  bool look_stop_ = false;

  Dispatcher* dispatcher_;
  std::mutex* lock_;
  CaptureSource* captures_;
  const Credentials* credentials_ = nullptr;
  Subscriptions events_;
  Clients clients_;
  net::Server server_;
  std::string token_;
  std::string shell_dir_;
  // The shell directory as the listener's one read root, apart from the methods' roots
  // (protocol.md, "The host's file roots"): `GET /shell/<path>` is admitted against it and nothing
  // else, so the shell serves no other file and gains the device-name, trailing-dot and link rules.
  io::Roots shell_roots_;
  std::string session_;
  // Tickets not yet spent, and the cookies spent ones bought: both the operator's, both held only
  // here, and both read by the server's threads, so under their own lock.
  struct Ticket {
    std::string value;
    i64 expires_unix_ms = 0;
  };
  std::mutex auth_mutex_;
  i64 ticket_lifetime_ms_ = k_ticket_lifetime_ms;
  Vector<Ticket> tickets_;
  Vector<std::string> cookies_;
};

}  // namespace engine::protocol
