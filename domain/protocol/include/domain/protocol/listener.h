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
// start and refuses every upgrade that does not carry it (`?token=` or an `X-Engine-Token` header)
// with close code 1008. An upgrade from a browser must also come from the listener's own origin,
// and every request must name the listener's own address in `Host`, which stops a page elsewhere
// from using DNS rebinding to reach the port. This is loopback and a shared secret, **not
// authentication of a user**: whoever holds the token is whoever the roles file says a call is, as
// on stdin (protocol.md, "Roles, leases and proposals").

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/protocol/rpc.h>
#include <foundation/net/server.h>

#include <mutex>
#include <string>
#include <string_view>

namespace engine::protocol {

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

class Listener final : public net::ServerHandler {
 public:
  // `dispatch_lock` is the host's own: every request runs under it. `captures` may be null.
  Listener(Dispatcher& dispatcher, std::mutex& dispatch_lock,
           CaptureSource* captures = nullptr) noexcept
      : dispatcher_(&dispatcher), lock_(&dispatch_lock), captures_(captures) {}
  ENGINE_NON_COPYABLE(Listener);
  ~Listener() override { stop(); }

  // Makes the token and starts serving on 127.0.0.1:`port` (0: a free port). `shell_dir` is the
  // directory the shell's static files are served from (empty or missing: `GET /` is a 404 that
  // says so); `session` is the session the host opened at start, named in the shell's URL.
  bool start(u16 port, std::string shell_dir, std::string session, std::string& error);
  void stop() { server_.stop(); }
  bool running() const noexcept { return server_.port() != 0; }

  u16 port() const noexcept { return server_.port(); }
  const std::string& token() const noexcept { return token_; }
  const std::string& session() const noexcept { return session_; }
  // The shell's address with the token and the session:
  // http://127.0.0.1:<port>/?token=...&session=...
  std::string shell_url() const;
  // The one line a host prints on stdout before it serves:
  // {"listen":"ws://127.0.0.1:<port>/","token":"...","session":"...","shell":"http://..."}.
  std::string announcement() const;

  void http(const net::HttpRequest& request, net::HttpResponse& response) override;
  bool accept(const net::HttpRequest& request, u16& close, std::string& reason) override;
  std::string message(u64 connection, std::string_view text) override;

 private:
  bool host_allowed(const net::HttpRequest& request) const;
  bool token_given(const net::HttpRequest& request) const;
  void serve_static(std::string_view relative, net::HttpResponse& response) const;
  void serve_capture(std::string_view rest, net::HttpResponse& response);

  Dispatcher* dispatcher_;
  std::mutex* lock_;
  CaptureSource* captures_;
  net::Server server_;
  std::string token_;
  std::string shell_dir_;
  std::string session_;
};

}  // namespace engine::protocol
