// Plan 06 §6.9's `place_asset` and `instantiate_template` (docs/subsystems/protocol.md, "Placing
// an asset, instantiating a template"): compound edits an agent would otherwise spell as a batch of
// CreateObject commands with ids and transforms worked out by hand. Each is one `doc.apply` (or a
// staging call into an open transaction), so roles, proposals, leases, the journal and undo treat
// it exactly as they treat the commands it compiles to.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <core/time/time.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>

#include <cstring>
#include <schemas/scene.h>
#include <string>

namespace engine::protocol {

namespace {

constexpr std::string_view k_placement_type = "engine.scene.MeshPlacement";
constexpr std::string_view k_origin_type = "engine.protocol.TemplateOrigin";

// A generated id: a function of what the call names alone, so the same call makes the same ids
// and a test can say which.
Id128 derived_id(Id128 a, u64 b, u64 salt) {
  u64 words[3] = {a.hi, a.lo, b};
  const u64 hi = hash_bytes(words, sizeof(words), 0x9e3779b97f4a7c15ull ^ salt);
  const u64 lo = hash_bytes(words, sizeof(words), 0xc2b2ae3d27d4eb4full ^ salt);
  return Id128::from_parts(hi, lo == 0 && hi == 0 ? 1 : lo);
}

std::string hex_of(Id128 id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return std::string(hex, Id128::k_hex_length);
}

bool ends_with(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  for (usize i = 0; i < suffix.size(); ++i) {
    char c = s[s.size() - suffix.size() + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

// What the record's `mesh` names, read far enough to say it is that kind of file: a .glb's and a
// .clusters' magic, a .gltf's JSON with its `asset.version`. The record keeps the path as given.
bool check_asset(const Session& s, std::string_view asset, std::string& kind, u64& bytes,
                 RpcError& error) {
  if (asset.empty()) {
    error = make_error(codes::k_invalid_argument, "asset is required: a .gltf, .glb or .clusters");
    return false;
  }
  if (ends_with(asset, ".gltf")) {
    kind = "gltf";
  } else if (ends_with(asset, ".glb")) {
    kind = "glb";
  } else if (ends_with(asset, ".clusters")) {
    kind = "clusters";
  } else {
    error = make_error(codes::k_invalid_argument,
                       "asset " + std::string(asset) +
                           " is not a mesh a placement can name: a .gltf, .glb or .clusters file");
    return false;
  }
  const bool absolute =
      asset.size() > 1 && (asset[0] == '/' || asset[0] == '\\' || asset[1] == ':');
  const std::string path =
      absolute ? std::string(asset) : doc::DocumentStore::path_of(s.dir(), asset);
  // The header's four bytes and the size: the asset may be hundreds of megabytes, and nothing here
  // needs more of it.
  io::FileInfo info;
  if (io::stat_file(path, info) != io::Status::Ok || info.is_directory) {
    error = make_error(codes::k_not_found, "no asset at " + path);
    return false;
  }
  bytes = info.size;
  char head[4] = {};
  u64 got = 0;
  bool ok = false;
  if (kind == "gltf") {
    // A .gltf is JSON naming its `asset.version`; the buffers it points at are the importer's.
    std::string text;
    ok = io::read_file(path, text) == io::Status::Ok;
    JsonValue value;
    ok = ok && parse_json(text, value).ok;
    const JsonValue* a = ok ? value.find("asset") : nullptr;
    ok = a != nullptr && a->find("version") != nullptr;
  } else if (io::read_file_range(path, 0, head, sizeof(head), got) == io::Status::Ok && got == 4) {
    ok = std::memcmp(head, kind == "glb" ? "glTF" : "CLST", 4) == 0;
  }
  if (!ok) {
    error = make_error(codes::k_invalid_argument,
                       "asset " + path + " does not read as a " + kind + " file");
    return false;
  }
  return true;
}

// The commands, through the session's checked write: committed at once, or staged.
bool write(Session& s, std::span<const doc::Command> commands, const Caller& caller,
           std::string_view layer, u64 transaction, ApplyResult& out, RpcError& error) {
  if (transaction != 0)
    return s.stage(transaction, commands, caller, true, layer, time::wall_unix_ms(), out, error);
  return s.apply(commands, caller, true, layer, out, error);
}

bool place_asset(Context& ctx, const PlaceAssetParams& params, PlaceResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx.policy, &params.attribution, caller, error)) return false;
  if (params.placements.empty()) {
    error = make_error(codes::k_invalid_argument, "placements is empty: nothing to place");
    return false;
  }
  if (!check_asset(*s, params.asset, out.kind, out.bytes, error)) return false;
  // Generated ids are a function of the seed, the asset and the placement's index: the same call
  // places the same ids, and another asset under the same seed others.
  const u64 asset_key = hash_bytes(params.asset.data(), params.asset.size());
  Vector<doc::Command> commands;
  commands.reserve(params.placements.size());
  for (u32 i = 0; i < params.placements.size(); ++i) {
    const AssetPlacement& p = params.placements[i];
    scene::MeshPlacement record;
    record.name = p.name;
    record.mesh = params.asset;
    record.translation = p.translation;
    record.rotation = p.rotation;
    record.yaw_deg = p.yaw_deg;
    record.scale = p.scale;
    doc::Command c;
    c.kind = doc::CommandKind::CreateObject;
    c.id = p.id.is_null() ? derived_id(Id128::from_parts(params.seed, asset_key), i, 1) : p.id;
    c.type = std::string(k_placement_type);
    c.parent = p.parent;
    c.value = schema::to_json(record);
    out.ids.push_back(c.id);
    commands.push_back(std::move(c));
  }
  return write(*s, {commands.data(), commands.size()}, caller, params.layer, params.transaction,
               out.apply, error);
}

// A property of a record's type whose values are world positions: `worldpos`, or an optional one.
bool is_world_position(const schema::TypeInfo* type, std::string_view property) {
  if (type == nullptr) return false;
  const schema::FieldInfo* f = type->find_field(property);
  if (f == nullptr) return false;
  const schema::TypeRef* t = &f->type;
  if (t->kind == schema::Kind::Optional && t->element != nullptr) t = t->element;
  return t->kind == schema::Kind::WorldPos;
}

bool offset_point(JsonValue& v, const DVec3& offset) {
  if (!v.is_array() || v.size() != 3) return false;
  const f64 d[3] = {offset.x, offset.y, offset.z};
  for (u32 i = 0; i < 3; ++i) {
    f64 x = 0;
    if (!v[i].get_f64(x)) return false;
    v[i] = JsonValue(x + d[i]);
  }
  return true;
}

bool instantiate_template(Context& ctx, const InstantiateTemplateParams& params, PlaceResult& out,
                          RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx.policy, &params.attribution, caller, error)) return false;
  const doc::Document& d = s->document();
  if (!d.exists(params.root)) {
    error =
        make_error(codes::k_not_found, "no live object " + hex_of(params.root) + " to instantiate");
    return false;
  }
  // The template and its live descendants, depth first, children by id: an order that is the
  // document's alone. A template origin under the template is not copied: it is the template's
  // own history, not its content.
  Vector<Id128> sources;
  Vector<Id128> stack;
  stack.push_back(params.root);
  while (!stack.empty()) {
    const Id128 id = stack.back();
    stack.pop_back();
    doc::ResolvedObject r;
    if (!d.resolve(id, r) || r.deleted || r.type == k_origin_type) continue;
    sources.push_back(id);
    if (sources.size() > params.max_records) {
      error =
          make_error(codes::k_invalid_argument,
                     "template " + hex_of(params.root) + " has more than " +
                         std::to_string(params.max_records) +
                         " live records (max_records); raise it, or instantiate a smaller part");
      return false;
    }
    const Vector<Id128> children = d.children(id);
    for (u32 i = children.size(); i > 0; --i)
      stack.push_back(children[i - 1]);
  }
  const Id128 root = params.id.is_null() ? derived_id(params.root, params.seed, 2) : params.id;
  // A copy's id is a function of the copy's root and its source alone.
  const auto copy_of = [&](Id128 source) {
    return source == params.root ? root : derived_id(root, source.hi ^ source.lo, 3);
  };
  Vector<doc::Command> commands;
  commands.reserve(sources.size() + 1);
  TemplateOrigin origin;
  origin.source_root = params.root;
  for (u32 i = 0; i < sources.size(); ++i) {
    doc::ResolvedObject r;
    (void)d.resolve(sources[i], r);
    const schema::TypeInfo* type = schema::Registry::global().find(r.type);
    JsonValue value = JsonValue::object();
    for (const auto& [name, v] : r.properties) {
      JsonValue copy = *v;
      if (is_world_position(type, name)) (void)offset_point(copy, params.offset);
      value.set(name, std::move(copy));
    }
    doc::Command c;
    c.kind = doc::CommandKind::CreateObject;
    c.id = copy_of(sources[i]);
    c.type = std::string(r.type);
    if (i == 0) {
      if (!params.parent.is_null()) c.parent = params.parent;
    } else {
      c.parent = copy_of(r.parent);
    }
    c.value = std::move(value);
    origin.sources.push_back(sources[i]);
    origin.copies.push_back(c.id);
    out.ids.push_back(c.id);
    commands.push_back(std::move(c));
  }
  if (params.record_origin) {
    doc::Command c;
    c.kind = doc::CommandKind::CreateObject;
    c.id = derived_id(root, 0, 4);
    c.type = std::string(k_origin_type);
    c.parent = root;
    // Where the copy's root stands, so a lease or a role on its tile covers its origin too.
    c.value = schema::to_json(origin);
    if (const JsonValue* p = commands[0].value.find("position"); p != nullptr && p->is_array())
      c.value.set("position", *p);
    commands.push_back(std::move(c));
  }
  return write(*s, {commands.data(), commands.size()}, caller, params.layer, params.transaction,
               out.apply, error);
}

}  // namespace

void add_placement_methods(Dispatcher& d) {
  d.add(method<PlaceAssetParams, PlaceResult, &place_asset>(
      "doc.place_asset",
      "Place a mesh asset (.gltf, .glb or .clusters, checked to read as one) any number of times "
      "as engine.scene.MeshPlacement records, in one transaction or staged into an open one."));
  d.add(method<InstantiateTemplateParams, PlaceResult, &instantiate_template>(
      "doc.instantiate_template",
      "Copy an object and its live descendants under fresh ids, moved by an offset, with a "
      "TemplateOrigin record of where the copy came from; one transaction, or staged."));
}

}  // namespace engine::protocol
