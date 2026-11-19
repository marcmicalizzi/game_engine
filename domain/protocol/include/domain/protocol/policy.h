#pragma once

// Roles (docs/plan/06-agent-tooling.md §6.5; docs/subsystems/protocol.md, "Roles, leases and
// proposals"): configuration that says what a call may change.
//
// A role is a row of a roles file (`engine-host --roles <file.json>`, schema `RolesFile`): the
// writing methods a call in it may make, the layers whose content it may change (directly, or
// through a proposal that targets them), the tiles and object types it may touch, and whether its
// work needs review. Director, designer, environment, QA and performance are rows, not code
// (`content/roles/roles.json`).
//
// **For the operator and stdio, roles are guardrails; for a credential, a boundary.** A caller the
// host trusts as its operator (stdin, the printed token, the shell's cookie) names its role in its
// attribution and the host believes it: what a role buys there is that an agent told to be the QA
// agent cannot, by mistake, rewrite the terrain, and that the refusal says so in a structured error
// (1008 Forbidden) a model can act on. A connection bound to a credential (clients.h, ADR-0069) is
// its credential's role and nothing else, on every host that accepts the credential: with no roles
// file to define that role, or no role named, it may only read (`bound_role`). Where one agent owns
// one host — engine-mcp starts a host per agent — the host is given that agent's identity
// (`--actor`, `--role`, `--task`), every call that names none runs in it, and the bridge refuses a
// call that names another.
//
// **Read access is never restricted.** A method registered `read_only` (rpc.h) passes whatever the
// role; a role lists only the writing methods it may make.
//
// No roles file restricts nothing for the operator, which is every host before roles existed, and
// leaves a credential's connection only its reads.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>
#include <domain/protocol/rpc.h>

#include <schemas/protocol.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::protocol {

struct Client;  // clients.h

// '*' matches any run of characters, none included; every other character matches itself; the
// whole of `text` must match. "doc.*" matches "doc.apply"; "*" matches everything.
bool glob_match(std::string_view pattern, std::string_view text) noexcept;
bool any_glob_match(const Vector<std::string>& patterns, std::string_view text) noexcept;

bool tile_in(const TileRange& range, doc::TileCoord tile) noexcept;
bool ranges_overlap(const TileRange& a, const TileRange& b) noexcept;
// `inner` lies inside one of `outer`.
bool range_within(const TileRange& inner, const Vector<TileRange>& outer) noexcept;

// The host's role configuration and its default identity. A Context without one (a null
// `Context::policy`) restricts nothing and fills nothing.
class Policy {
 public:
  // Reads a roles file's text. Checks that every role has a unique, non-empty name, that every
  // pattern is non-empty, that every range has x0 <= x1 and y0 <= y1, and that `default_role`
  // names a role; false with a sentence naming the first problem otherwise.
  bool load_roles(std::string_view path, std::string_view text, std::string& error);
  bool roles_loaded() const noexcept { return loaded_; }
  const std::string& roles_path() const noexcept { return path_; }
  const RolesFile& roles() const noexcept { return roles_; }
  const Role* find_role(std::string_view name) const noexcept;

  // What a call that names no actor, role or task is attributed to and checked as: the host's
  // `--actor`, `--role` and `--task`. A `--role` must name a role of the loaded file (the host
  // checks that at startup).
  doc::Attribution identity;

  // `given` with its blank actor, role and task taken from `identity`.
  doc::Attribution fill(doc::Attribution given) const;

  // The role a call runs in: `named` when it is not blank, then `identity.role`, then the file's
  // `default_role`. Sets `out` to null when nothing restricts the call (no file, or no name at
  // all); false with 1008 Forbidden when the name is not a role of the file, since a call that
  // claims a role nobody configured must not fall through to no restriction at all. **A role
  // marked human is never resolved here** (1008, `data.reason` "human review"): this is how a call
  // with no credential is placed, and a call's attribution, a connection's hello and a host's start
  // line are all claims anyone can type (ADR-0072). Only `resolve_caller` confers one, from a
  // credential bound to it.
  bool resolve_role(std::string_view named, const Role*& out, RpcError& error) const;

  // Whether `name` is a role of the loaded file marked human: what a host's or engine-cli's start
  // line is refused for (`--role`, exit 2) before anything starts.
  bool is_human_role(std::string_view name) const noexcept;

  // The dispatcher's gate: a method that is not read-only must match one of the role's `methods`.
  // `named_role` is the role the call's params name (`attribution.role`), possibly empty.
  bool allows_method(const MethodDesc& method, std::string_view named_role, RpcError& error) const;

 private:
  RolesFile roles_;
  std::string path_;
  bool loaded_ = false;
};

// Who is making a call, resolved once per call: its attribution with the connection's and then
// the host's defaults filled in, the role it runs in (null: nothing restricts it), and the client
// connection it came on (0: stdio or in process), which a lease it takes and the events its
// changes publish carry.
//
// **How a caller is known to be a person is one field, read in one place.** `human_via` is the
// name of the credential that confers a role marked human (ADR-0072), empty for everyone else, and
// `human_role` that role's name; both are set only by `resolve_caller`, and `Role::human` is read
// only in policy.cpp (`policy_tests.cpp` fails when another source reads it), so a check written
// by copying a person's check reads these and never the working role's flag. `via` is how the
// caller came to be who it says, recorded beside every ledger write: "credential:<name>" for a
// connection bound to an issued credential, "operator" for a connection on the printed token or
// the shell's cookie (whose actor is its own claim), "stdio" for stdin, engine-cli, a console
// script and the host's own in-process calls.
struct Caller {
  doc::Attribution who;
  const Role* role = nullptr;
  u64 connection = 0;
  std::string via;
  std::string human_via;
  std::string human_role;
};

// `given` null means the call carried no attribution. Blank actor, role and task come from the
// connection's `client.hello` (`ctx.clients`), then from the host's identity (`ctx.policy`, which
// may be null). False with 1008 when the call names a role the roles file does not have. A
// connection bound to a credential takes its blank fields from the credential alone, never from
// the host, and runs in `bound_role` (ADR-0069).
bool resolve_caller(const Context& ctx, const doc::Attribution* given, Caller& out,
                    RpcError& error);
// The connection's own identity (`client.hello`) when it gave one, else null: what fills a call's
// blank attribution before the host's.
const doc::Attribution* client_identity(const Context& ctx) noexcept;
// A connection bound to a credential (clients.h, ADR-0069) is its credential's actor and role: a
// call that names another (either may be blank, which is the credential's) is refused with 1008,
// `data.reason` "identity", before its handler runs. True for every other caller.
bool identity_allows(const Context& ctx, std::string_view actor, std::string_view role,
                     RpcError& error);

// **A role a host cannot enforce is never a label** (ADR-0069). The connection's client entry
// when it is bound to a credential, else null.
const Client* bound_client(const Context& ctx) noexcept;
// The role a bound connection runs in: its credential's role as the roles file defines it, and
// nothing else — never the host's `--role` or the file's `default_role`. A credential that names no
// role, or one whose role the host cannot find (no roles file loaded, as on engine-editor), runs in
// a role that names no method, layer or type: its reads pass, every write is refused.
const Role& bound_role(const Policy* policy, std::string_view name) noexcept;
// The dispatcher's gate for a bound connection, whether or not a roles file is loaded: a method
// that is not read-only must be one of its role's `methods`. 1008 Forbidden otherwise, with
// `data.reason` "method", "no role" (the credential names none) or "unenforced role" (this host
// cannot enforce the one it names).
bool bound_allows_method(const Context& ctx, const Client& client, const MethodDesc& method,
                         RpcError& error);

// The same gate's second half, for a read whose call leaves a file (rpc.h, `call_leaves_files`;
// protocol.md, "What a read may do"): a bound connection that may only read — `bound_role` is the
// role with no methods — is refused it, 1008 with `data.reason` "no role" or "unenforced role";
// an enforced role passes.
bool bound_allows_files(const Context& ctx, const Client& client, const MethodDesc& method,
                        RpcError& error);

// 1008 Forbidden with `data.reason` and `data.role`.
RpcError forbidden(std::string message, std::string_view reason, std::string_view role,
                   JsonValue extra = {});

// Where an object stands, as the lease and role checks see it: whether some layer defines it, its
// composed type, and the tile its composed position falls in on a grid (the lease layer's
// partition). An object with no position, or on a layer with no partition, has no tile.
struct Place {
  bool defined = false;
  bool tiled = false;
  doc::TileCoord tile;
  // The registry's own copy of the type name when the type is registered, so it outlives the
  // record it was read from; `type_owned` holds it otherwise (an unknown type, which validation
  // reports anyway).
  const char* type_registered = nullptr;
  std::string type_owned;

  std::string_view type() const noexcept {
    return type_registered != nullptr ? std::string_view(type_registered)
                                      : std::string_view(type_owned);
  }
};

// Composed, never per record: an override that sets a colour and not a position still changes
// an object standing somewhere, and "a lease on tile (3, 7)" means the objects standing in it.
// The composed position is what `doc.get` shows. Allocates nothing for a registered type.
void place_of(const doc::Document& document, doc::ObjectId id, const doc::LayerPartition& partition,
              Place& out);

}  // namespace engine::protocol
