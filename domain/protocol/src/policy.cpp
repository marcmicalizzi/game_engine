// Roles (policy.h): a roles file, the host's default identity, and the checks a call's role makes.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/doc/partition.h>
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
  if (out != nullptr) return true;
  error = forbidden("no role named '" + std::string(name) + "' in the host's roles file (" + path_ +
                        "): a call must name a configured role",
                    "unknown role", name);
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

bool resolve_caller(const Policy* policy, const doc::Attribution* given, Caller& out,
                    RpcError& error) {
  out.who = given != nullptr ? *given : doc::Attribution{};
  out.role = nullptr;
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
