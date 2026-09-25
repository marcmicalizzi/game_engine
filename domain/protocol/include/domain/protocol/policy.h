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
// **Roles are guardrails for cooperating agents, not security.** The protocol has no
// authentication: a call names its role in its attribution, and the host believes it. What a role
// buys is that an agent told to be the QA agent cannot, by mistake, rewrite the terrain — and that
// the refusal says so, in a structured error (1008 Forbidden) a model can act on. Where one agent
// owns one host — engine-mcp starts a host per agent — the host is given that agent's identity
// (`--actor`, `--role`, `--task`), every call that names none runs in it, and the bridge refuses a
// call that names another.
//
// **Read access is never restricted.** A method registered `read_only` (rpc.h) passes whatever the
// role; a role lists only the writing methods it may make.
//
// No roles file restricts nothing, which is every host before roles existed.

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
  // claims a role nobody configured must not fall through to no restriction at all.
  bool resolve_role(std::string_view named, const Role*& out, RpcError& error) const;

  // The dispatcher's gate: a method that is not read-only must match one of the role's `methods`.
  // `named_role` is the role the call's params name (`attribution.role`), possibly empty.
  bool allows_method(const MethodDesc& method, std::string_view named_role, RpcError& error) const;

 private:
  RolesFile roles_;
  std::string path_;
  bool loaded_ = false;
};

// Who is making a call, resolved once per call: its attribution with the host's defaults filled
// in, and the role it runs in (null: nothing restricts it).
struct Caller {
  doc::Attribution who;
  const Role* role = nullptr;
};

// `given` null means the call carried no attribution. Policy may be null. False with 1008 when the
// call names a role the roles file does not have.
bool resolve_caller(const Policy* policy, const doc::Attribution* given, Caller& out,
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
