#pragma once

// engine-host's `--listen` (docs/subsystems/apps.md, "engine-host --listen"; protocol.md,
// "Transport"): the protocol over a loopback WebSocket, beside the stdio loop and through the same
// dispatcher, plus the few HTTP GETs a browser shell needs — its static files and the captures
// this process kept. Built on foundation/net, which can bind 127.0.0.1 and nothing else.
//
// **Indistinguishable from stdin.** Every text message is one JSON-RPC request (or batch) handed to
// `protocol::Dispatcher::dispatch_text` under the same lock the stdio loop takes, so attribution,
// roles, the read-only rule and the journal see a socket's request exactly as they see a line on
// stdin. The lock also makes the host's state — sessions, the renderer, the worlds — single-
// threaded as it always was: a second connection is *served* concurrently (its messages are read
// and answered without waiting for the first to close) but its requests run one at a time with
// everyone else's.
//
// **The token.** Loopback stops the network, not the machine: any local process — and any web page
// the user's browser has open, since a browser lets a page open a WebSocket to 127.0.0.1 — can
// reach the port. So the host makes a 128-bit token from the system's secure random source at
// start, prints it once on stdout, and refuses every upgrade that does not carry it (`?token=` or
// an `X-Engine-Token` header) with close code 1008. An upgrade from a browser must also come from
// the host's own origin, and every request must name the host's own address in `Host`, which
// stops a page elsewhere from using DNS rebinding to reach the port. This is loopback and a shared
// secret, **not authentication of a user**: whoever holds the token is whoever the roles file says
// a call is, as on stdin (protocol.md, "Roles, leases and proposals").

#include "host_state.h"

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/protocol/rpc.h>
#include <foundation/net/server.h>

#include <mutex>
#include <string>
#include <string_view>

namespace engine::host {

class Listen final : public net::ServerHandler {
 public:
  Listen(protocol::Dispatcher& dispatcher, std::mutex& dispatch_lock, HostState& state) noexcept
      : dispatcher_(&dispatcher), lock_(&dispatch_lock), state_(&state) {}
  ENGINE_NON_COPYABLE(Listen);
  ~Listen() override { stop(); }

  // Makes the token and starts serving on 127.0.0.1:`port`. `shell_dir` is the directory the
  // shell's static files are served from (empty or missing: `GET /` is a 404 that says so);
  // `session` is the session `--doc` opened, named in the URL the announcement prints.
  bool start(u16 port, std::string shell_dir, std::string session, std::string& error);
  void stop() { server_.stop(); }

  u16 port() const noexcept { return server_.port(); }
  const std::string& token() const noexcept { return token_; }
  // The one line the host prints on stdout before it serves:
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

  protocol::Dispatcher* dispatcher_;
  std::mutex* lock_;
  HostState* state_;
  net::Server server_;
  std::string token_;
  std::string shell_dir_;
  std::string session_;
};

}  // namespace engine::host
