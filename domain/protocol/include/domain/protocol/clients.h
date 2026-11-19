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
// every call repeating its attribution. From the operator's token it is a declaration, not a
// proof, exactly as a call's attribution is; a connection that presented a credential (below) is
// bound to the credential's actor, role and task instead, and `client.hello` changes only its name.
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
#include <domain/protocol/policy.h>

#include <schemas/protocol.h>
#include <span>
#include <string>
#include <string_view>

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
  // How it authenticated (ADR-0069): "operator", or the name of its credential. Never the token.
  std::string credential;
  // `who` is its credential's and no call may name another actor or role.
  bool bound = false;
};

// ---- credentials (roadmap A4, ADR-0069; protocol.md, "Who a connection is") -------------------

// A token: 128 bits from the system's secure random source, base64url without padding (22
// characters). False when the source did not answer.
bool make_token(std::string& out);
// What the credentials file keeps of a token: its SHA-256 (core/hash/sha256.h), 64 lowercase hex
// digits.
std::string token_digest(std::string_view token);
// Equal strings, compared without stopping at the first difference, so how long a refusal takes
// says nothing about how much of a guess was right.
bool same_secret(std::string_view a, std::string_view b) noexcept;

// The credentials a host accepts beside its own printed token (`engine-host --credentials`).
class Credentials {
 public:
  // Reads a credentials file's text: every entry has a unique non-empty name, an actor, and a
  // digest of 64 lowercase hex digits (SHA-256); a role, when given, must be one of `policy`'s
  // roles file when it loaded one. False with a sentence naming the first problem otherwise, never
  // quoting a digest. A role the host cannot check here (no roles file) is not refused: it binds
  // all the same, and its connection may only read (policy.h, `bound_role`).
  bool load(std::string_view path, std::string_view text, const Policy* policy, std::string& error);
  // The credential `token` is, or null.
  const Credential* match(std::string_view token) const;
  bool loaded() const noexcept { return !file_.credentials.empty(); }
  const CredentialsFile& file() const noexcept { return file_; }
  // How many credentials may only read on a host with this policy: those naming no role, and,
  // with no roles file loaded, every one. What a host says once at start, so its operator learns
  // then and not at an agent's first write that a role it issued does not hold here.
  u32 read_only_count(const Policy* policy) const noexcept;

 private:
  CredentialsFile file_;
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
