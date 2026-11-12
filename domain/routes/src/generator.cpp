// The plan's file and the placement generator "routes" (docs/subsystems/routes.md, "One plan" and
// "In a scene"): the network decided once and kept under the derived-data root, read by the entry
// when a scene is, as the channels' plan is.
#include "detail.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>

namespace engine::routes {

namespace {

constexpr u32 k_plan_magic = 0x4C505452u;  // "RTPL"
constexpr u32 k_plan_format = 1;
// The lattice points past which sampling asks the host for its job pool: 256 x 256.
constexpr f64 k_pool_points = 65536.0;
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
  w.put(n.fords);
  w.put(n.ford_m);
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
  n.fords = r.get<u32>();
  n.ford_m = r.get<f64>();
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
  // No plan named: **the plan stands on the ground before any edit** (`Context::uncut()`;
  // scene_gen.md, "What an entry plans for the ground"), which is the context's own ground where
  // the host is asking for the plan or the scene has no edits, and the terrain less its edits where
  // the ground already carries this entry's treads. So the open that hands the treads over and the
  // open on the benched ground read the same heights and make one key. A hit samples the lattice
  // and plans nothing.
  const scene_gen::Ground ground = context.uncut();
  // **The host's job pool only for a lattice that repays starting it** (routes.md, "Budgets"). A
  // host makes its pool the first time a generator asks, and starting and stopping one cost a
  // scene's read 0.16 to 0.67 s on a busy 36-thread machine, where the 3,969 points of a 124 m
  // region are sampled serially within the 5 ms the entry's whole open takes. The lattice's bytes
  // do not depend on who samples it, so the threshold moves no plan.
  const f64 spacing = static_cast<f64>(layer.spacing_m);
  const f64 points = ((layer.region_max[0] - layer.region_min[0]) / spacing + 1.0) *
                     ((layer.region_max[1] - layer.region_min[1]) / spacing + 1.0);
  jobs::JobSystem* const pool = points > k_pool_points ? context.jobs() : nullptr;
  RouteNetwork n;
  if (!detail::sample_lattice(layer, ground, pool, n, error, context.exclusion)) return false;
  u64 ground_hash = hash_bytes(n.ground_mm.data(), n.ground_mm.size() * sizeof(i64));
  // The water each lattice point stands in (routes.md, "Water"), when the host handed any.
  if (!n.wet.empty())
    ground_hash = hash_combine(ground_hash, hash_bytes(n.wet.data(), n.wet.size()));
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
  if (!detail::plan_from_lattice(layer, ground, n, error, context.exclusion)) return false;
  out.key = key;
  out.layer_hash = layer_hash;
  out.network = std::move(n);
  out.planned = true;
  // Best effort: a directory that cannot be written costs the next load its planning, not this one.
  (void)io::write_file_atomic(plan_path(dir, key), plan_bytes(out));
  return true;
}

namespace {

// A band filed under a tile it reaches: sorted by (z, x, band), so a tile's bands are one run in
// the network's order.
struct TileBand {
  i64 z = 0;
  i64 x = 0;
  u32 band = 0;
  constexpr bool operator<(const TileBand& o) const noexcept {
    return z != o.z ? z < o.z : (x != o.x ? x < o.x : band < o.band);
  }
};
struct Entry {
  RouteLayer layer;
  RoutePlan plan;
  // **The tread as shapes** (routes.md, "In a scene"): its bands, and each filed under every tile
  // of the world's grid it stands on or within the wear's reach of, which is what `tile` hands and
  // `occupies` answers from.
  Vector<scene_gen::Footprint> bands;
  Vector<TileBand> by_tile;
  i64 tile_mm = 32000;
};

bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
constexpr const char* k_moving = "routes require static ground; a moving ground keeps no tread";

constexpr i64 floor_div(i64 a, i64 b) noexcept { return a / b - ((a % b != 0 && (a < 0)) ? 1 : 0); }

// Files every band under the tiles its disc, grown by the wear's reach, touches. Integers past
// the two products that turn metres into millimetres, so the same tiles on every toolchain.
void file_bands(Entry& e, f32 tile_size) {
  e.bands.clear();
  e.by_tile.clear();
  tread_footprints(e.layer, e.plan.network, e.bands);
  e.tile_mm = static_cast<i64>(static_cast<f64>(tile_size) * 1000.0 + 0.5);
  if (e.tile_mm < 1) e.tile_mm = 1;
  const i64 reach_mm = static_cast<i64>(static_cast<f64>(e.layer.wear_m) * 1000.0 + 0.5);
  for (u32 k = 0; k < e.bands.size(); ++k) {
    const scene_gen::Footprint& band = e.bands[k];
    const i64 r = static_cast<i64>(band.radius * 1000.0) + 1 + reach_mm;
    const i64 x0 = floor_div(band.x_mm - r, e.tile_mm), x1 = floor_div(band.x_mm + r, e.tile_mm);
    const i64 z0 = floor_div(band.z_mm - r, e.tile_mm), z1 = floor_div(band.z_mm + r, e.tile_mm);
    for (i64 z = z0; z <= z1; ++z)
      for (i64 x = x0; x <= x1; ++x)
        e.by_tile.push_back(TileBand{z, x, k});
  }
  std::sort(e.by_tile.begin(), e.by_tile.end());
}
// The first band filed under the tile, and one past the last.
std::pair<u32, u32> tile_run(const Entry& e, scene_gen::TileCoord tile) noexcept {
  const TileBand lo{tile.z, tile.x, 0};
  const TileBand* const begin = e.by_tile.data();
  const TileBand* const end = begin + e.by_tile.size();
  const TileBand* first = std::lower_bound(begin, end, lo);
  const TileBand* last = first;
  while (last != end && last->z == tile.z && last->x == tile.x)
    ++last;
  return {static_cast<u32>(first - begin), static_cast<u32>(last - begin)};
}

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
  file_bands(*e, context.tile_size);
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
// **The treads, handed to the ground** (routes.md, "In a scene"; scene_gen.md, "What an entry
// plans for the ground"): each line's `Bench` edit, read back from the JSON `engine-content
// routes` writes, so a tread planned at load and the same tread pasted into the scene's
// `terrain.tiles.edits` are the same edit, field for field, and build the same tile set.
bool plan_ground(void* state, const scene_gen::Context& c, scene_gen::GroundPlan& out,
                 std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const Entry& e = *static_cast<const Entry*>(state);
  const JsonValue edits = tread_edits_json(e.layer, e.plan.network);
  if (!edits.is_array()) return true;
  for (const JsonValue& json : edits.as_array()) {
    scene::TerrainEdit edit;
    schema::ReadContext reader;
    if (!schema::from_json(edit, json, reader) || !reader.ok())
      return refused(error,
                     "routes: a tread's edit did not read back as an engine.scene.TerrainEdit");
    out.edits.push_back(std::move(edit));
  }
  return true;
}
// No mesh is placed yet — a route's steps, roots and stones are later rows' — but the tread is
// handed as the shapes it occupies: every band for a whole read, and for a tile the bands filed
// under it, so the entries after this one keep off the tread.
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const Entry& e = *static_cast<const Entry*>(state);
  out.shapes.add(std::span<const scene_gen::Footprint>(e.bands.data(), e.bands.size()));
  return true;
}
bool meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>&, std::string*) {
  return true;
}
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const Entry& e = *static_cast<const Entry*>(state);
  const auto [first, last] = tile_run(e, at);
  for (u32 k = first; k < last; ++k)
    out.shapes.add(e.bands[e.by_tile[k].band]);
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord at) noexcept {
  const auto [first, last] = tile_run(*static_cast<const Entry*>(state), at);
  return first != last;
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_routes{.name = "routes",
                                                     .open = &open,
                                                     .close = &close,
                                                     .expand = &expand,
                                                     .meshes = &meshes,
                                                     .tile = &tile,
                                                     .occupies = &occupies,
                                                     .representation = &representation,
                                                     .plan_ground = &plan_ground};
const scene_gen::Registrar k_registrar{k_routes};

}  // namespace
}  // namespace engine::routes
