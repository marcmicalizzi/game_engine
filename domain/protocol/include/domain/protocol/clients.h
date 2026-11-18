#pragma once

// The clients of one host (docs/subsystems/protocol.md, "Several clients, one host"; plan 06 §6.3;
// roadmap A3): every socket connection the listener serves, who it said it is, and how many
// requests it has made. The editor's shell, an agent's bridge attached with `engine-mcp --connect`
// and a script on the host's stdin are three clients of one document at once.
//
// **Who a connection is.** `client.hello {name, attribution}` names the connection's actor, role
// and task, and every later call on it that leaves one of them blank is filled from here before
// the host's own `--actor`, `--role` and `--task` (policy.h, `resolve_caller`; the dispatcher's
// role gate reads the role the same way). That is what makes "each its own caller" true without
// every call repeating its attribution. It is a declaration, not a proof: until a credential
// carries identity (roadmap A4), a connection is who it says it is, exactly as a call is.
//
// **What a connection holds** is kept where it is used rather than here: the sessions it opened
// are holders in the session manager (session.h), and the leases it took carry its number
// (`LeaseInfo.connection`). When the connection ends the listener gives both back under the
// dispatch lock (listener.h, `closed`), and forgets the connection here.
//
// Not synchronized: written and read only under the host's dispatch lock, like everything a
// method reaches through its `Context`.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/doc/document.h>

#include <span>
#include <string>

namespace engine::protocol {

// `client.hello`'s name is free text an operator reads; longer is refused.
inline constexpr u32 k_client_name_max = 64;

struct Client {
  u64 connection = 0;
  std::string name;
  // Actor, role and task only; rationale and timestamp belong to a transaction.
  doc::Attribution who;
  bool identified = false;
  i64 connected_unix_ms = 0;
  u64 requests = 0;
};

class Clients {
 public:
  Clients() = default;
  ENGINE_NON_COPYABLE(Clients);

  // The connection's entry, made the first time it is seen (its first request) with `now_ms` as
  // when it connected.
  Client& touch(u64 connection, i64 now_ms);
  Client* find(u64 connection) noexcept;
  const Client* find(u64 connection) const noexcept;
  // The connection has ended.
  void forget(u64 connection);
  // In connection order: connections are numbered as they are accepted.
  std::span<const Client> clients() const noexcept { return {clients_.data(), clients_.size()}; }

 private:
  Vector<Client> clients_;
};

}  // namespace engine::protocol
