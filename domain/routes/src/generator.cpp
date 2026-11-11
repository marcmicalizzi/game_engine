// The plan's file and the placement generator "routes" (docs/subsystems/routes.md, "One plan" and
// "In a scene"): the network decided once and kept under the derived-data root, read by the entry
// when a scene is, as the channels' plan is.
#include "detail.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

namespace engine::routes {

namespace {

constexpr u32 k_plan_magic = 0x4C505452u;  // "RTPL"
constexpr u32 k_plan_format = 1;
static_assert(sizeof(Station) == 32, "a station is written as its bytes: no padding");

struct Writer {
  std::string bytes;
  template <class T>
  void put(const T& v) {
    bytes.append(reinterpret_cast<const char*>(&v), sizeof(T));
  }
  template <class T>
  void list(const Vector<T>& v) {
    put(static_cast<u32>(v.size()));
    if (!v.empty()) bytes.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
  }
};
struct Reader {
  const char* at;
  const char* end;
  bool ok = true;
  template <class T>
  T get() {
    T v{};
    if (!ok || static_cast<usize>(end - at) < sizeof(T)) {
      ok = false;
      return v;
    }
    std::memcpy(&v, at, sizeof(T));
    at += sizeof(T);
    return v;
  }
  template <class T>
  void list(Vector<T>& v) {
    const u32 count = get<u32>();
    if (!ok || static_cast<usize>(end - at) / sizeof(T) < count) {
      ok = false;
      return;
    }
    v.resize(count);
    if (count > 0) std::memcpy(v.data(), at, static_cast<usize>(count) * sizeof(T));
    at += static_cast<usize>(count) * sizeof(T);
  }
};
u64 parse_key(std::string_view text, bool& ok) {
  u64 key = 0;
  ok = text.size() == 16;
  for (const char c : text) {
    const u32 digit = c >= '0' && c <= '9'   ? static_cast<u32>(c - '0')
                      : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a') + 10
                                             : 16u;
    if (digit > 15) ok = false;
    key = key << 4 | (digit & 15u);
  }
  return key;
}

}  // namespace

u64 plan_layer_hash(const RouteLayer& layer) {
  RouteLayer copy = layer;
  copy.plan.clear();
  JsonWriteOptions compact;
  compact.pretty = false;
  const std::string text = write_json(schema::to_json(copy), compact);
  return hash_combine(hash_bytes(text.data(), text.size()), k_routes_version);
}
std::string plan_key_text(u64 key) {
  char name[24];
  std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
  return name;
}
std::string plan_path(std::string_view dir, u64 key) {
  return io::join_path(dir, "plan-" + plan_key_text(key) + ".route-plan");
}
bool derived_dir(std::string_view ddc, std::string_view scene_dir, std::string& out,
                 std::string& error) {
  std::string root(ddc);
  if (root.empty()) {
    std::error_code ec;
    const auto from = std::filesystem::absolute(
        scene_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(scene_dir), ec);
    if (!ec) root = geometry::find_ddc_root(from.generic_string());
    if (root.empty()) root = io::join_path(scene_dir, "ddc");
  }
  std::error_code ec;
  out = std::filesystem::absolute(io::join_path(root, "routes"), ec).generic_string();
  if (ec || io::make_directories(out) != io::Status::Ok) {
    error = "routes: cannot make the derived-data directory " + out;
    return false;
  }
  return true;
}

std::string plan_bytes(const RoutePlan& plan) {
  Writer w;
  const RouteNetwork& n = plan.network;
  w.put(k_plan_magic);
  w.put(k_plan_format);
  w.put(k_routes_version);
  w.put(static_cast<u32>(n.lines.size()));
  w.put(plan.key);
  w.put(plan.layer_hash);
  w.put(n.width);
  w.put(n.depth);
  w.put(n.x_mm);
  w.put(n.z_mm);
  w.put(n.spacing_mm);
  w.put(n.legs);
  w.put(n.steep_legs);
  w.put(n.unreached);
  w.put(n.switchbacks);
  w.put(n.length_m);
  w.put(n.over_cap_m);
  for (const RouteLine& l : n.lines) {
    w.list(l.cells);
    w.list(l.stations);
    w.put(l.length_m);
    w.put(l.steep_steps);
    w.put(l.switchbacks);
    w.put(l.over_cap_m);
    w.put(l.max_hold_m);
  }
  w.put(hash_bytes(w.bytes.data(), w.bytes.size()));
  return std::move(w.bytes);
}

bool read_plan(std::string_view bytes, RoutePlan& out, std::string& error) {
  out = {};
  const auto refuse = [&](const char* why) {
    out = {};
    error = std::string("routes: the plan file ") + why;
    return false;
  };
  if (bytes.size() < 4 * sizeof(u32) + sizeof(u64)) return refuse("is too short to be one");
  Reader r{bytes.data(), bytes.data() + bytes.size() - sizeof(u64)};
  if (r.get<u32>() != k_plan_magic) return refuse("is not a route plan");
  if (r.get<u32>() != k_plan_format || r.get<u32>() != k_routes_version)
    return refuse("was written by another version of the routes capability");
  u64 closing = 0;
  std::memcpy(&closing, r.end, sizeof(u64));
  if (closing != hash_bytes(bytes.data(), bytes.size() - sizeof(u64)))
    return refuse("does not hash to what it closes with");
  const u32 lines = r.get<u32>();
  RouteNetwork& n = out.network;
  out.key = r.get<u64>();
  out.layer_hash = r.get<u64>();
  n.width = r.get<u32>();
  n.depth = r.get<u32>();
  n.x_mm = r.get<i64>();
  n.z_mm = r.get<i64>();
  n.spacing_mm = r.get<i64>();
  n.legs = r.get<u32>();
  n.steep_legs = r.get<u32>();
  n.unreached = r.get<u32>();
  n.switchbacks = r.get<u32>();
  n.length_m = r.get<f64>();
  n.over_cap_m = r.get<f64>();
  // A line is at least its scalars: a count the bytes cannot hold is refused before it is sized.
  if (!r.ok || static_cast<usize>(r.end - r.at) / 40 < lines)
    return refuse("names more lines than it holds");
  n.lines.resize(lines);
  const u64 cells = static_cast<u64>(n.width) * n.depth;
  for (RouteLine& l : n.lines) {
    r.list(l.cells);
    r.list(l.stations);
    l.length_m = r.get<f64>();
    l.steep_steps = r.get<u32>();
    l.switchbacks = r.get<u32>();
    l.over_cap_m = r.get<f64>();
    l.max_hold_m = r.get<f64>();
    if (!r.ok) return refuse("is not the length its contents say");
    for (const u32 c : l.cells)
      if (c >= cells) return refuse("holds a line off its lattice");
  }
  if (!r.ok || r.at != r.end) return refuse("is not the length its contents say");
  return true;
}

bool ensure_plan(const RouteLayer& layer, const scene_gen::Context& context, std::string_view dir,
                 RoutePlan& out, std::string& error) {
  out = {};
  if (!layer_in_range(layer, error)) return false;
  const u64 layer_hash = plan_layer_hash(layer);
  const auto read = [&](u64 key, bool& found) {
    std::string bytes;
    found = io::read_file(plan_path(dir, key), bytes) == io::Status::Ok;
    return found && read_plan(bytes, out, error);
  };
  const std::string remake =
      "; run `engine-content routes <scene> <out>` over the ground before its treads and paste its "
      "edits and the entry's plan again";
  if (!layer.plan.empty()) {
    bool ok = false;
    const u64 key = parse_key(layer.plan, ok);
    if (!ok) {
      error =
          "engine.routes: invalid plan (16 hexadecimal digits, as engine-content routes prints)";
      return false;
    }
    bool found = false;
    if (!read(key, found)) {
      if (found) return false;
      error = "routes: the plan " + layer.plan + " the entry names is not in " + std::string(dir) +
              remake;
      return false;
    }
    if (out.key != key || out.layer_hash != layer_hash) {
      out = {};
      error = "routes: the plan " + layer.plan + " was made from another layer than this entry's" +
              remake;
      return false;
    }
    return true;
  }
  // No plan named: the ground is the ground before any tread, and the key is what the plan reads.
  // A hit samples the lattice and plans nothing.
  RouteNetwork n;
  if (!detail::sample_lattice(layer, context.ground, context.jobs(), n, error)) return false;
  const u64 ground_hash = hash_bytes(n.ground_mm.data(), n.ground_mm.size() * sizeof(i64));
  const u64 key =
      hash_combine(hash_combine(layer_hash, ground_hash), 0x52545054504C414Eull);  // "RTPTPLAN"
  bool found = false;
  RoutePlan cached;
  {
    std::string bytes;
    found = io::read_file(plan_path(dir, key), bytes) == io::Status::Ok;
    std::string why;
    if (found && read_plan(bytes, cached, why) && cached.key == key &&
        cached.layer_hash == layer_hash) {
      out = std::move(cached);
      return true;
    }
  }
  if (!detail::plan_from_lattice(layer, context.ground, n, error)) return false;
  out.key = key;
  out.layer_hash = layer_hash;
  out.network = std::move(n);
  out.planned = true;
  // Best effort: a directory that cannot be written costs the next load its planning, not this one.
  (void)io::write_file_atomic(plan_path(dir, key), plan_bytes(out));
  return true;
}

namespace {

struct Entry {
  RouteLayer layer;
  RoutePlan plan;
};

bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
constexpr const char* k_moving = "routes require static ground; a moving ground keeps no tread";

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  if (!schema::from_json(e->layer, params, reader) || !reader.ok()) {
    std::string message = "engine.routes.RouteLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  if (context.stand.present()) return refused(error, k_moving);
  std::string why, root;
  if (!derived_dir(context.ddc, context.dir, root, why)) return refused(error, why);
  // **The plan, never made again over its own treads** (routes.md, "One plan"): read from its file
  // when the entry names it, made here only for an entry that names none.
  if (!ensure_plan(e->layer, context, root, e->plan, why)) return refused(error, why);
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
// Nothing is placed yet: the tread is the ground's (an edit and a mask region the scene carries),
// and a route's meshes — steps, roots and stones in the tread — are later rows'.
bool expand(void*, const scene_gen::Context& c, scene_gen::Placements&, std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  return true;
}
bool meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>&, std::string*) {
  return true;
}
bool tile(void*, scene_gen::TileCoord, const scene_gen::Context& c, scene_gen::Placements&,
          std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  return true;
}
bool occupies(const void*, scene_gen::TileCoord) noexcept { return false; }
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_routes{.name = "routes",
                                                     .open = &open,
                                                     .close = &close,
                                                     .expand = &expand,
                                                     .meshes = &meshes,
                                                     .tile = &tile,
                                                     .occupies = &occupies,
                                                     .representation = &representation};
const scene_gen::Registrar k_registrar{k_routes};

}  // namespace
}  // namespace engine::routes
