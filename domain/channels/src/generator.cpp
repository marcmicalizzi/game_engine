// The placement generator "channels" (docs/subsystems/channels.md, "In a scene"): the network over
// the entry's region decided once, when the entry is opened, its pieces kept as recipes, and each
// tile handed the pieces whose origin it holds with their recipes (W97b: a tile's bed is built when
// the tile activates, never at scene read) — so a tile's placements and meshes are the same
// whenever and in whatever order it comes in.
#include "detail.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

namespace engine::channels {

// ---- the plan and its file (channels.md, "One plan") -------------------------------------------

namespace {
using namespace detail;

constexpr u32 k_plan_magic = 0x4C504843u;  // "CHPL"
constexpr u32 k_plan_format = 2;           // 2 (W97): a link's sediment
static_assert(sizeof(ChannelPoint) == 40, "a point is written as its bytes: no padding");
static_assert(sizeof(ChannelSediment) == 32, "a sediment is written as its bytes: no padding");

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
// Every field of a link, in the header's order; one function both ways would hide which is which.
void write_link(Writer& w, const ChannelLink& l) {
  w.list(l.cells);
  w.list(l.line);
  w.put(l.length_m);
  w.put(l.slope);
  w.put(l.meander);
  w.put(static_cast<u8>(l.head));
  w.put(l.seed);
  w.put(l.area_end);
  w.list(l.reach);
  w.list(l.step_depth);
  w.list(l.bed);
  w.list(l.step_at);
  w.list(l.step_height);
  w.list(l.cluster_at);
  w.list(l.cluster_size);
  w.list(l.knick);
  w.put(l.knickpoints);
  w.put(l.lip_convexity);
  w.put(l.steep_left);
  w.put(l.trough_pulled);
  w.put(l.bank_moved);
  w.list(l.asym);
  w.list(l.swell);
  w.list(l.flood_left);
  w.list(l.flood_right);
  w.list(l.entrenchment);
  w.list(l.sediment);
  w.put(l.keep);
  w.put(l.trunk);
  w.put(static_cast<u8>(l.mouth));
  w.put(static_cast<u8>(l.outlet));
  w.put(l.mouth_level);
}
bool read_link(Reader& r, ChannelLink& l) {
  r.list(l.cells);
  r.list(l.line);
  l.length_m = r.get<f64>();
  l.slope = r.get<f64>();
  l.meander = r.get<f64>();
  l.head = r.get<u8>() != 0;
  l.seed = r.get<u64>();
  l.area_end = r.get<u32>();
  r.list(l.reach);
  r.list(l.step_depth);
  r.list(l.bed);
  r.list(l.step_at);
  r.list(l.step_height);
  r.list(l.cluster_at);
  r.list(l.cluster_size);
  r.list(l.knick);
  l.knickpoints = r.get<u32>();
  l.lip_convexity = r.get<f64>();
  l.steep_left = r.get<u32>();
  l.trough_pulled = r.get<u32>();
  l.bank_moved = r.get<u32>();
  r.list(l.asym);
  r.list(l.swell);
  r.list(l.flood_left);
  r.list(l.flood_right);
  r.list(l.entrenchment);
  r.list(l.sediment);
  l.keep = r.get<u32>();
  l.trunk = r.get<u32>();
  l.mouth = r.get<u8>() != 0;
  l.outlet = r.get<u8>() != 0;
  l.mouth_level = r.get<f64>();
  if (!r.ok) return false;
  // What every reader of a link indexes by a point of its line is one a point, or absent.
  const u32 m = l.line.size();
  const auto per_point = [m](u32 size) { return size == 0 || size == m; };
  if (!per_point(l.reach.size()) || !per_point(l.step_depth.size()) || !per_point(l.bed.size()) ||
      !per_point(l.knick.size()) || !per_point(l.asym.size()) || !per_point(l.swell.size()) ||
      !per_point(l.flood_left.size()) || !per_point(l.flood_right.size()) ||
      !per_point(l.entrenchment.size()) || !per_point(l.sediment.size()) ||
      l.flood_left.size() != l.flood_right.size())
    return false;
  if (l.step_at.size() != l.step_height.size() || l.cluster_at.size() != l.cluster_size.size() ||
      l.keep > m)
    return false;
  for (const u32 i : l.step_at)
    if (i >= m) return false;
  for (const u32 i : l.cluster_at)
    if (i >= m) return false;
  return true;
}
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

u64 plan_layer_hash(const ChannelLayer& layer) {
  ChannelLayer copy = layer;
  copy.plan.clear();
  JsonWriteOptions compact;
  compact.pretty = false;
  const std::string text = write_json(schema::to_json(copy), compact);
  return hash_combine(hash_bytes(text.data(), text.size()), k_channels_version);
}
std::string plan_key_text(u64 key) {
  char name[24];
  std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
  return name;
}
std::string plan_path(std::string_view dir, u64 key) {
  return io::join_path(dir, "plan-" + plan_key_text(key) + ".channel-plan");
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
  out = std::filesystem::absolute(io::join_path(root, "channels"), ec).generic_string();
  if (ec || io::make_directories(out) != io::Status::Ok) {
    error = "channels: cannot make the derived-data directory " + out;
    return false;
  }
  return true;
}

std::string plan_bytes(const ChannelPlan& plan) {
  Writer w;
  const ChannelNetwork& n = plan.network;
  w.put(k_plan_magic);
  w.put(k_plan_format);
  w.put(k_channels_version);
  w.put(static_cast<u32>(n.links.size()));
  w.put(plan.key);
  w.put(plan.layer_hash);
  w.put(plan.seed);
  w.put(n.width);
  w.put(n.depth);
  w.put(n.x_mm);
  w.put(n.z_mm);
  w.put(n.spacing_mm);
  w.put(n.length_m);
  w.put(n.water.mouths);
  w.put(n.water.dropped);
  w.put(n.water.outlets);
  w.put(n.water.confluences);
  w.put(n.water.points_removed);
  w.list(n.heads);
  for (const ChannelLink& l : n.links)
    write_link(w, l);
  w.put(hash_bytes(w.bytes.data(), w.bytes.size()));
  return std::move(w.bytes);
}
bool read_plan(std::string_view bytes, ChannelPlan& out, std::string& error) {
  out = {};
  const auto refuse = [&](const char* why) {
    out = {};
    error = std::string("channels: the plan file ") + why;
    return false;
  };
  if (bytes.size() < 4 * sizeof(u32) + sizeof(u64)) return refuse("is too short to be one");
  Reader r{bytes.data(), bytes.data() + bytes.size() - sizeof(u64)};
  if (r.get<u32>() != k_plan_magic) return refuse("is not a channel plan");
  if (r.get<u32>() != k_plan_format || r.get<u32>() != k_channels_version)
    return refuse("was written by another version of the channels capability");
  u64 closing = 0;
  std::memcpy(&closing, r.end, sizeof(u64));
  if (closing != hash_bytes(bytes.data(), bytes.size() - sizeof(u64)))
    return refuse("does not hash to what it closes with");
  const u32 links = r.get<u32>();
  ChannelNetwork& n = out.network;
  out.key = r.get<u64>();
  out.layer_hash = r.get<u64>();
  out.seed = r.get<u64>();
  n.width = r.get<u32>();
  n.depth = r.get<u32>();
  n.x_mm = r.get<i64>();
  n.z_mm = r.get<i64>();
  n.spacing_mm = r.get<i64>();
  n.length_m = r.get<f64>();
  n.water.mouths = r.get<u32>();
  n.water.dropped = r.get<u32>();
  n.water.outlets = r.get<u32>();
  n.water.confluences = r.get<u32>();
  n.water.points_removed = r.get<u64>();
  r.list(n.heads);
  // A link is at least its scalars: a count the bytes cannot hold is refused before it is sized.
  if (!r.ok || static_cast<usize>(r.end - r.at) / 64 < links)
    return refuse("names more links than it holds");
  n.links.resize(links);
  for (ChannelLink& l : n.links)
    if (!read_link(r, l)) return refuse("holds a link whose arrays disagree");
  for (const ChannelLink& l : n.links)
    if (l.trunk != ~0u && l.trunk >= links) return refuse("holds a link whose trunk is no link");
  if (!r.ok || r.at != r.end) return refuse("is not the length its contents say");
  return true;
}

bool ensure_plan(const ChannelLayer& layer, const scene_gen::Context& context, std::string_view dir,
                 ChannelPlan& out, std::string& error) {
  out = {};
  if (!layer_in_range(layer, error)) return false;
  const u64 layer_hash = plan_layer_hash(layer);
  const auto read = [&](u64 key, bool& found) {
    std::string bytes;
    found = io::read_file(plan_path(dir, key), bytes) == io::Status::Ok;
    return found && read_plan(bytes, out, error);
  };
  const std::string remake =
      "; run `engine-content channels <scene> <out> --cut` and paste its cuts, its water_channels "
      "and the entry's plan again";
  if (!layer.plan.empty()) {
    // A named plan: the entry stands on ground its plan's cuts lowered, and plans nothing.
    bool ok = false;
    const u64 key = parse_key(layer.plan, ok);
    if (!ok) return fail(error, "plan (16 hexadecimal digits, as engine-content channels prints)");
    bool found = false;
    if (!read(key, found)) {
      if (found) return false;
      error = "channels: the plan " + layer.plan + " the entry names is not in " +
              std::string(dir) + remake;
      return false;
    }
    if (out.key != key || out.layer_hash != layer_hash || out.seed != context.world_seed) {
      out = {};
      error = "channels: the plan " + layer.plan +
              " was made from another layer or another world seed than this entry's" + remake;
      return false;
    }
    return true;
  }
  // No plan named: the ground is the ground before any cut, and the key is what the plan reads.
  ChannelNetwork n;
  jobs::JobSystem* jobs = context.jobs();
  if (!sample_lattice(layer, context.ground, jobs, n, error)) return false;
  u64 ground_hash = hash_bytes(n.ground_um.data(), n.ground_um.size() * sizeof(i64));
  if (context.exclusion != nullptr) {
    // The water bodies end a link at a shoreline: whether one covers each lattice point.
    Vector<u8> covered(n.ground_um.size(), 0);
    for (u32 i = 0; i < covered.size(); ++i)
      covered[i] =
          context.exclusion->excluded_mm(
              context.ground, n.x_mm + static_cast<i64>(i % n.width) * n.spacing_mm,
              n.z_mm + static_cast<i64>(i / n.width) * n.spacing_mm, scene_gen::k_exclude_water)
              ? 1
              : 0;
    ground_hash = hash_combine(ground_hash, hash_bytes(covered.data(), covered.size()));
  }
  const u64 key = hash_combine(
      hash_combine(hash_combine(layer_hash, context.world_seed), ground_hash), 0x4348504C414Eull);
  bool found = false;
  if (read(key, found) && out.key == key && out.layer_hash == layer_hash &&
      out.seed == context.world_seed)
    return true;
  // A miss, or a file that is not this plan (a torn write, another version): made again.
  out = {};
  error.clear();
  if (!plan_from_lattice(layer, context.ground, context.world_seed, n, error, context.exclusion))
    return false;
  out.key = key;
  out.layer_hash = layer_hash;
  out.seed = context.world_seed;
  out.network = std::move(n);
  out.planned = true;
  // Best effort: a directory that cannot be written costs the next load its planning, not this one.
  (void)io::write_file_atomic(plan_path(dir, key), plan_bytes(out));
  return true;
}

namespace {

// **What `open` keeps** (W97b; channels.md, "A tile's bed, laid when it activates"): the plan's
// decisions and nothing built — each piece's recipe text, its placement and its tile. No mesh is
// listed as resident and no file is written: a tile hands its own piece's recipe when it activates,
// and the host builds it off the frame's thread through the derived-data cache (scene_gen.md, "A
// tile's meshes").
struct Entry {
  ChannelLayer layer;
  Vector<scene_gen::PlacementMesh> meshes;  // one a piece, its recipe in memory
  Vector<scene_gen::Placement> placements;  // one a piece; `mesh` its index in `meshes`
  Vector<scene_gen::TileCoord> tiles;       // per placement
};

bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
constexpr const char* k_moving =
    "channels require static ground; a moving ground has no drainage to dress";

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  if (!schema::from_json(e->layer, params, reader) || !reader.ok()) {
    std::string message = "engine.channels.ChannelLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  if (context.stand.present()) return refused(error, k_moving);
  std::string why;
  std::string root;
  if (!derived_dir(context.ddc, context.dir, root, why)) return refused(error, why);
  // **The plan, never made again over cut ground** (channels.md, "One plan"): read from its file
  // when the entry names it or its key is cached, and made here only for an entry that names none,
  // whose ground is then the ground before any cut. The scene's water bodies reach it through the
  // exclusion the host built (W90; null: no mouths).
  ChannelPlan plan;
  if (!ensure_plan(e->layer, context, root, plan, why)) return refused(error, why);
  Vector<PlacedPiece> pieces;
  if (!place_pieces(e->layer, plan.network, context.ground, context.tile_size, pieces, why))
    return refused(error, why);
  for (const PlacedPiece& piece : pieces) {
    std::string text = piece_json(piece.recipe);
    char name[64];
    std::snprintf(name, sizeof(name), "channel-%016llx.channel.json",
                  static_cast<unsigned long long>(piece_source_hash(piece.recipe)));
    // The recipe is handed, not written: `path` is where its file would be, which names it.
    scene_gen::PlacementMesh mesh(io::join_path(root, name), std::string("channels/") + name, 0,
                                  std::move(text), piece_reach_m(piece.recipe));
    scene_gen::Placement at;
    at.position = piece.origin;
    at.mesh = e->meshes.size();
    at.id = piece.id;
    e->placements.push_back(at);
    e->tiles.push_back(piece.tile);
    e->meshes.push_back(std::move(mesh));
  }
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const auto& e = *static_cast<Entry*>(state);
  const u32 offset = out.meshes.size();
  for (const auto& m : e.meshes)
    out.meshes.push_back(m);
  for (const auto& p : e.placements) {
    auto at = p;
    at.mesh += offset;
    out.instances.push_back(at);
    ++out.things;
  }
  return true;
}
// A tile's piece arrives with the tile (`Placements::meshes`): nothing is resident before it, so
// the entry lists no `meshes` at all, and its placement indexes the list it is handed with.
bool tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const auto& e = *static_cast<Entry*>(state);
  for (u32 i = 0; i < e.placements.size(); ++i)
    if (e.tiles[i] == tile) {
      auto at = e.placements[i];
      at.mesh = out.meshes.size();
      out.meshes.push_back(e.meshes[e.placements[i].mesh]);
      out.instances.push_back(at);
      ++out.things;
    }
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  for (const auto& t : static_cast<const Entry*>(state)->tiles)
    if (t == tile) return true;
  return false;
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_channels{.name = "channels",
                                                       .open = &open,
                                                       .close = &close,
                                                       .expand = &expand,
                                                       .tile = &tile,
                                                       .occupies = &occupies,
                                                       .representation = &representation,
                                                       .tile_meshes = true};
const scene_gen::Registrar k_registrar{k_channels};
}  // namespace
}  // namespace engine::channels
