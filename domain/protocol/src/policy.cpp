// Roles (policy.h): a roles file, the host's default identity, and the checks a call's role makes.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/doc/partition.h>
#include <domain/protocol/clients.h>
#include <domain/protocol/policy.h>

namespace engine::protocol {

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
  // Iterative with one backtrack point: the classic linear-time matcher for a single wildcard kind.
  usize p = 0, t = 0;
  usize star = std::string_view::npos, mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (p < pattern.size() && pattern[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*')
    ++p;
  return p == pattern.size();
}

bool any_glob_match(const Vector<std::string>& patterns, std::string_view text) noexcept {
  for (const std::string& p : patterns) {
    if (glob_match(p, text)) return true;
  }
  return false;
}

bool tile_in(const TileRange& range, doc::TileCoord tile) noexcept {
  return tile.x >= range.x0 && tile.x <= range.x1 && tile.y >= range.y0 && tile.y <= range.y1;
}

bool ranges_overlap(const TileRange& a, const TileRange& b) noexcept {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

bool range_within(const TileRange& inner, const Vector<TileRange>& outer) noexcept {
  for (const TileRange& o : outer) {
    if (inner.x0 >= o.x0 && inner.x1 <= o.x1 && inner.y0 >= o.y0 && inner.y1 <= o.y1) return true;
  }
  return false;
}

RpcError forbidden(std::string message, std::string_view reason, std::string_view role,
                   JsonValue extra) {
  JsonValue data = extra.is_object() ? std::move(extra) : JsonValue::object();
  data.set("reason", JsonValue(reason));
  data.set("role", JsonValue(role));
  return make_error(codes::k_forbidden, std::move(message), std::move(data));
}

// ---- Policy -----------------------------------------------------------------------------------

namespace {

bool check_patterns(const Role& role, const Vector<std::string>& list, const char* what,
                    std::string& error) {
  for (const std::string& p : list) {
    if (p.empty()) {
      error = "role '" + role.name + "' has an empty entry in " + what;
      return false;
    }
  }
  return true;
}

}  // namespace

bool Policy::load_roles(std::string_view path, std::string_view text, std::string& error) {
  JsonValue value;
  if (const JsonParseResult r = parse_json(text, value); !r.ok) {
    error = std::string(path) + ": " + r.message;
    return false;
  }
  RolesFile file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, value, ctx)) {
    error = std::string(path) + ": ";
    if (!ctx.diagnostics.empty()) {
      if (!ctx.diagnostics[0].path.empty()) error += ctx.diagnostics[0].path + ": ";
      error += ctx.diagnostics[0].message;
    }
    return false;
  }
  for (u32 i = 0; i < file.roles.size(); ++i) {
    const Role& role = file.roles[i];
    if (role.name.empty()) {
      error = std::string(path) + ": role " + std::to_string(i) + " has no name";
      return false;
    }
    for (u32 j = 0; j < i; ++j) {
      if (file.roles[j].name == role.name) {
        error = std::string(path) + ": role '" + role.name + "' is defined twice";
        return false;
      }
    }
    if (!check_patterns(role, role.methods, "methods", error) ||
        !check_patterns(role, role.layers, "layers", error) ||
        !check_patterns(role, role.object_types, "object_types", error)) {
      error = std::string(path) + ": " + error;
      return false;
    }
    for (const TileRange& r : role.tiles) {
      if (r.x0 > r.x1 || r.y0 > r.y1) {
        error = std::string(path) + ": role '" + role.name +
                "' has a tile range whose low corner is above its high one";
        return false;
      }
    }
  }
  roles_ = std::move(file);
  if (!roles_.default_role.empty() && find_role(roles_.default_role) == nullptr) {
    error = std::string(path) + ": default_role '" + roles_.default_role + "' is not a role";
    roles_ = RolesFile{};
    return false;
  }
  // Every call with no credential would run in it, and no such call is a person's.
  if (const Role* d = find_role(roles_.default_role); d != nullptr && d->human) {
    error = std::string(path) + ": default_role '" + roles_.default_role +
            "' is marked human, and a person's role is conferred only by a credential (ADR-0072)";
    roles_ = RolesFile{};
    return false;
  }
  path_ = std::string(path);
  loaded_ = true;
  return true;
}

const Role* Policy::find_role(std::string_view name) const noexcept {
  for (const Role& r : roles_.roles) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

namespace {

bool blank(std::string_view s) noexcept {
  return s.find_first_not_of(" \t") == std::string_view::npos;
}

}  // namespace

doc::Attribution Policy::fill(doc::Attribution given) const {
  if (blank(given.actor)) given.actor = identity.actor;
  if (blank(given.role)) given.role = identity.role;
  if (blank(given.task)) given.task = identity.task;
  return given;
}

bool Policy::resolve_role(std::string_view named, const Role*& out, RpcError& error) const {
  out = nullptr;
  if (!loaded_) return true;
  std::string_view name = named;
  if (blank(name)) name = identity.role;
  if (blank(name)) name = roles_.default_role;
  if (blank(name)) return true;
  out = find_role(name);
  if (out != nullptr && out->human) {
    out = nullptr;
    error = forbidden("role '" + std::string(name) +
                          "' is marked human, and a call is a person's only by a credential the "
                          "host's operator issued for that role, presented on its connection: "
                          "naming the role (in an attribution, a hello or a start line) does not "
                          "make a call a person's (ADR-0072)",
                      "human review", name);
    return false;
  }
  if (out != nullptr) return true;
  error = forbidden("no role named '" + std::string(name) + "' in the host's roles file (" + path_ +
                        "): a call must name a configured role",
                    "unknown role", name);
  return false;
}

bool Policy::is_human_role(std::string_view name) const noexcept {
  const Role* role = loaded_ ? find_role(name) : nullptr;
  return role != nullptr && role->human;
}

namespace {

// What a bound connection whose role the host cannot enforce runs in: a role that names no method,
// no layer and no type, so the dispatcher's gate and every check a handler makes refuse its writes,
// and its reads pass as every read does (ADR-0069).
const Role& no_role() {
  static const Role k_none = [] {
    Role r;
    r.name = "(none)";
    return r;
  }();
  return k_none;
}

}  // namespace

const Role& bound_role(const Policy* policy, std::string_view name) noexcept {
  if (policy == nullptr || !policy->roles_loaded() || blank(name)) return no_role();
  const Role* role = policy->find_role(name);
  return role != nullptr ? *role : no_role();
}

const Client* bound_client(const Context& ctx) noexcept {
  if (ctx.clients == nullptr || ctx.connection == 0) return nullptr;
  const Client* client = ctx.clients->find(ctx.connection);
  return client != nullptr && client->bound ? client : nullptr;
}

bool bound_allows_method(const Context& ctx, const Client& client, const MethodDesc& method,
                         RpcError& error) {
  if (method.read_only) return true;
  const Role& role = bound_role(ctx.policy, client.who.role);
  if (&role != &no_role()) {
    if (any_glob_match(role.methods, method.name)) return true;
    JsonValue extra = JsonValue::object();
    extra.set("method", JsonValue(method.name));
    extra.set("credential", JsonValue(client.credential));
    error = forbidden("role '" + role.name + "' of the credential '" + client.credential +
                          "' may not call " + std::string(method.name) +
                          ": it is not among the role's methods (read-only methods are always "
                          "allowed)",
                      "method", role.name, std::move(extra));
    return false;
  }
  JsonValue extra = JsonValue::object();
  extra.set("method", JsonValue(method.name));
  extra.set("credential", JsonValue(client.credential));
  const bool has_role = !blank(client.who.role);
  const bool no_file = ctx.policy == nullptr || !ctx.policy->roles_loaded();
  std::string why;
  if (!has_role) {
    why = "names no role, so its connection may only read";
  } else if (no_file) {
    why = "names role '" + client.who.role +
          "', and this host loaded no roles file to enforce it, so its connection may only read";
  } else {
    why = "names role '" + client.who.role + "', which the host's roles file does not have";
  }
  error = forbidden("the credential '" + client.credential + "' " + why + ": " +
                        std::string(method.name) +
                        " writes. Ask the host's operator for a credential with a role of the "
                        "host's roles file (ADR-0069)",
                    has_role ? "unenforced role" : "no role", client.who.role, std::move(extra));
  return false;
}

bool bound_allows_files(const Context& ctx, const Client& client, const MethodDesc& method,
                        RpcError& error) {
  if (&bound_role(ctx.policy, client.who.role) != &no_role()) return true;
  JsonValue extra = JsonValue::object();
  extra.set("method", JsonValue(method.name));
  extra.set("credential", JsonValue(client.credential));
  const bool has_role = !blank(client.who.role);
  const bool no_file = ctx.policy == nullptr || !ctx.policy->roles_loaded();
  std::string why;
  if (!has_role) {
    why = "names no role";
  } else if (no_file) {
    why = "names role '" + client.who.role + "', and this host loaded no roles file to enforce it";
  } else {
    why = "names role '" + client.who.role + "', which the host's roles file does not have";
  }
  error = forbidden("the credential '" + client.credential + "' " + why +
                        ", so its connection may only read and nothing is written on its behalf: " +
                        std::string(method.name) +
                        " as called leaves a file on the host's disk. Ask the host's operator for "
                        "a credential with a role of the host's roles file (ADR-0069)",
                    has_role ? "unenforced role" : "no role", client.who.role, std::move(extra));
  return false;
}

bool Policy::allows_method(const MethodDesc& method, std::string_view named_role,
                           RpcError& error) const {
  if (method.read_only || !loaded_) return true;
  const Role* role = nullptr;
  if (!resolve_role(named_role, role, error)) return false;
  if (role == nullptr || any_glob_match(role->methods, method.name)) return true;
  JsonValue extra = JsonValue::object();
  extra.set("method", JsonValue(method.name));
  error = forbidden("role '" + role->name + "' may not call " + std::string(method.name) +
                        ": it is not among the role's methods (read-only methods are always "
                        "allowed)",
                    "method", role->name, std::move(extra));
  return false;
}

const doc::Attribution* client_identity(const Context& ctx) noexcept {
  if (ctx.clients == nullptr || ctx.connection == 0) return nullptr;
  const Client* client = ctx.clients->find(ctx.connection);
  return client != nullptr && client->identified ? &client->who : nullptr;
}

bool identity_allows(const Context& ctx, std::string_view actor, std::string_view role,
                     RpcError& error) {
  if (ctx.clients == nullptr || ctx.connection == 0) return true;
  const Client* client = ctx.clients->find(ctx.connection);
  if (client == nullptr || !client->bound) return true;
  const bool other_actor = !blank(actor) && actor != client->who.actor;
  const bool other_role = !blank(role) && role != client->who.role;
  if (!other_actor && !other_role) return true;
  JsonValue extra = JsonValue::object();
  extra.set("credential", JsonValue(client->credential));
  extra.set("actor", JsonValue(client->who.actor));
  error = forbidden(
      "this connection is " + client->who.actor +
          (client->who.role.empty() ? std::string() : " in role '" + client->who.role + "'") +
          " by the credential '" + client->credential +
          "' it presented, and may not "
          "call as " +
          (other_actor ? "actor " + std::string(actor) : "role " + std::string(role)) +
          ": leave attribution's actor and role blank, or ask the host's operator "
          "for a credential that is",
      "identity", client->who.role, std::move(extra));
  return false;
}

bool resolve_caller(const Context& ctx, const doc::Attribution* given, Caller& out,
                    RpcError& error) {
  out.who = given != nullptr ? *given : doc::Attribution{};
  out.role = nullptr;
  out.connection = ctx.connection;
  out.human_via.clear();
  out.human_role.clear();
  out.via = ctx.connection == 0 ? "stdio" : "operator";
  // A connection bound to a credential is its credential and nothing else (ADR-0069): the host's
  // own identity never fills it, and its role is the credential's as the roles file defines it, or
  // the role that may write nothing when the host cannot enforce one. The dispatcher's identity
  // gate looks only at a top-level `attribution` of object params, so this does not lean on it: a
  // named actor or role that is not the credential's is refused here too, and the role every
  // handler checks is the credential's, whatever the call said.
  if (const Client* bound = bound_client(ctx)) {
    if (!identity_allows(ctx, out.who.actor, out.who.role, error)) return false;
    if (blank(out.who.actor)) out.who.actor = bound->who.actor;
    if (blank(out.who.role)) out.who.role = bound->who.role;
    if (blank(out.who.task)) out.who.task = bound->who.task;
    out.role = &bound_role(ctx.policy, bound->who.role);
    out.via = "credential:" + bound->credential;
    // The one place a person is conferred: the operator issued this credential for a role the
    // roles file marks human, and the connection presented it.
    if (out.role->human) {
      out.human_via = bound->credential;
      out.human_role = out.role->name;
    }
    return true;
  }
  // The connection's identity first: several clients share one host, and each is who it said it
  // was, not who the host was started as.
  if (const doc::Attribution* client = client_identity(ctx)) {
    if (blank(out.who.actor)) out.who.actor = client->actor;
    if (blank(out.who.role)) out.who.role = client->role;
    if (blank(out.who.task)) out.who.task = client->task;
  }
  const Policy* policy = ctx.policy;
  if (policy == nullptr) return true;
  out.who = policy->fill(std::move(out.who));
  return policy->resolve_role(out.who.role, out.role, error);
}

// ---- Place ------------------------------------------------------------------------------------

void place_of(const doc::Document& document, doc::ObjectId id, const doc::LayerPartition& partition,
              Place& out) {
  out.defined = document.is_defined(id);
  out.tiled = false;
  out.type_registered = nullptr;
  out.type_owned.clear();
  if (!out.defined) return;
  const std::string_view type = document.type_of(id);
  if (!type.empty()) {
    const schema::TypeInfo* info = schema::Registry::global().find(type);
    if (info != nullptr) {
      out.type_registered = info->qualified_name;
    } else {
      out.type_owned.assign(type);
    }
  }
  const std::string_view property = doc::position_property(type, partition);
  if (property.empty()) return;
  const JsonValue* value = document.property(id, property);
  if (value == nullptr) return;
  out.tiled = doc::tile_of_position(*value, partition.tile_size, out.tile);
}

}  // namespace engine::protocol
