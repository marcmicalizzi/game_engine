// The scene generator "ledges" (rocks.md, "Exposed bedrock (G142)"): a placement generator
// through the scene-generator registry that stands the formation's resistant beds out of a
// region's slopes, one merged mesh a tile and form.
//
// **What it holds.** The plan above the tile is the entry itself: the formation, the rule and
// the region. A tile's patch is a function of that and the ground's heights over the tile and an
// apron, in whole millimetres, so the same world read from another start, or a tile at a time in
// any order, writes the same recipes.
//
// **What it cannot hold yet.** A streamed tile may name only a mesh `meshes` listed when the scene
// was read (scene_gen.h, `PlacementGeneratorDesc`), so `open` samples and solves every tile of the
// region to know which have ledges and writes their recipes; the region is bounded for that
// reason (`k_max_tiles`). The cost is the region's, not the world's, and rocks.md says what a
// tile-time mesh would change.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/ledges.h>
#include <domain/scene_gen/derived_root.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>

namespace engine::rocks {
namespace {

constexpr u32 k_none = ~0u;
// The most tiles a region may hold: each is sampled and solved at `open`.
constexpr u64 k_max_tiles = 16384;

struct TilePlace {
  scene_gen::TileCoord tile;
  WorldPos position{};
  u32 mesh[2] = {k_none, k_none};  // the near form's and the far form's
};
struct Entry {
  LedgeLayer layer;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<TilePlace> tiles;
};

i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return q - (a % b < 0 ? 1 : 0);
}
bool refused(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  if (!schema::from_json(e->layer, params, reader) || !reader.ok()) {
    std::string message = "engine.rocks.LedgeLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  const LedgeLayer& layer = e->layer;
  if (context.stand.present())
    return refused(error, "ledges require static ground; rebake for changed terrain");
  if (!(layer.near_rings <= 8 && layer.far_rings >= layer.near_rings && layer.far_rings <= 8))
    return refused(error, "engine.rocks.LedgeLayer: near_rings <= far_rings <= 8");
  for (u32 k = 0; k < 2; ++k)
    if (!std::isfinite(layer.region_min[k]) || !std::isfinite(layer.region_max[k]) ||
        std::fabs(layer.region_min[k]) > 1e8 || std::fabs(layer.region_max[k]) > 1e8 ||
        !(layer.region_max[k] > layer.region_min[k]))
      return refused(error,
                     "engine.rocks.LedgeLayer: region_min and region_max must be finite, "
                     "within 100,000 km, and max above min");
  if (!(context.tile_size >= 0.25f && context.tile_size <= 4096.0f))
    return refused(error, "ledges: invalid tile_size");
  const i64 tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  // A material's images are named relative to the scene; a recipe under the derived-data root
  // must name them absolutely.
  const auto absolute_image = [&](std::optional<std::string>& path) {
    if (path && !path->empty()) {
      std::error_code path_error;
      const auto input = std::filesystem::path(*path);
      const auto resolved = std::filesystem::absolute(
          input.is_absolute() ? input : std::filesystem::path(context.dir) / input, path_error);
      if (path_error) return false;
      *path = resolved.generic_string();
    }
    return true;
  };
  auto& material = e->layer.material;
  if (!absolute_image(material.base_color) || !absolute_image(material.normal) ||
      !absolute_image(material.roughness) || !absolute_image(material.metallic) ||
      !absolute_image(material.height))
    return refused(error, "ledges: cannot resolve material image path");

  std::string root;
  if (!layer.recipes.empty()) {
    root = io::join_path(context.dir, layer.recipes);
  } else {
    // The one rule (T119; scene_gen.md, "The derived-data root"): the host's root, else the scene's
    // own `ddc/`, which every host already hands as `Context::ddc`.
    root = io::join_path(scene_gen::host_derived_root(context.ddc, context.dir, true), "ledges");
  }
  std::error_code ec;
  root = std::filesystem::absolute(root, ec).generic_string();
  if (ec) return refused(error, "ledges: cannot resolve the recipes' directory");

  // The tiles whose corner lies in the region, rows of x in order of z.
  const i64 x0 = floor_div(scene_gen::nearest_mm(layer.region_min[0]) + tile_mm - 1, tile_mm);
  const i64 z0 = floor_div(scene_gen::nearest_mm(layer.region_min[1]) + tile_mm - 1, tile_mm);
  const i64 x1 = floor_div(scene_gen::nearest_mm(layer.region_max[0]) - 1, tile_mm);
  const i64 z1 = floor_div(scene_gen::nearest_mm(layer.region_max[1]) - 1, tile_mm);
  if (x1 < x0 || z1 < z0) {
    *state = e.release();  // a region smaller than a tile's corner spacing dresses nothing
    return true;
  }
  if (static_cast<u64>(x1 - x0 + 1) * static_cast<u64>(z1 - z0 + 1) > k_max_tiles)
    return refused(error,
                   "engine.rocks.LedgeLayer: the region holds more than 16,384 tiles "
                   "(each is solved when the scene opens)");
  if (x0 < std::numeric_limits<i32>::min() || x1 > std::numeric_limits<i32>::max() ||
      z0 < std::numeric_limits<i32>::min() || z1 > std::numeric_limits<i32>::max())
    return refused(error, "ledges: tile coordinate out of range");
  LedgeLayer keyed = layer;
  keyed.seed = hash_combine(layer.seed, context.world_seed);  // one seed a region, never a tile's
  for (i64 tz = z0; tz <= z1; ++tz)
    for (i64 tx = x0; tx <= x1; ++tx) {
      TilePlace place;
      place.tile = {static_cast<i32>(tx), static_cast<i32>(tz)};
      bool any = false;
      for (u32 form = 0; form < 2; ++form) {
        // A far form no ring draws is never built.
        if (form == 1 && layer.far_rings <= layer.near_rings) continue;
        LedgePatch patch;
        std::string why;
        if (!sample_ledge_patch(keyed, context.ground, tx * tile_mm, tz * tile_mm, tile_mm,
                                form == 1, patch, why))
          return refused(error, why);
        assets::MeshData mesh;
        if (!solve_ledges(patch, mesh, nullptr, why)) return refused(error, why);
        if (mesh.indices.empty()) continue;
        JsonWriteOptions compact;
        compact.pretty = false;
        const std::string text = write_json(schema::to_json(patch), compact);
        char name[64];
        std::snprintf(name, sizeof(name), "ledge-%016llx.ledge.json",
                      static_cast<unsigned long long>(ledge_source_hash(patch)));
        scene_gen::PlacementMesh named;
        named.path = io::join_path(root, name);
        named.name = std::string("ledges/") + name;
        if (io::make_directories(root) != io::Status::Ok ||
            (!io::exists(named.path) && io::write_file_atomic(named.path, text) != io::Status::Ok))
          return refused(error, "ledges: cannot write " + named.path);
        place.position = {static_cast<f64>(patch.x_mm) / 1000.0,
                          static_cast<f64>(patch.y_mm) / 1000.0,
                          static_cast<f64>(patch.z_mm) / 1000.0};
        place.mesh[form] = e->meshes.size();
        e->meshes.push_back(std::move(named));
        any = true;
      }
      if (any) e->tiles.push_back(place);
    }
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }

// The form `ring` draws: 0 near, 1 far, 2 nothing. A whole read is near.
u8 form_of(const Entry& e, const scene_gen::Context& c) {
  if (c.ring == scene_gen::k_no_ring) return 0;
  if (c.ring < e.layer.near_rings) return 0;
  return c.ring < e.layer.far_rings ? u8{1} : u8{2};
}
scene_gen::Placement placement_of(const TilePlace& t, u32 mesh, u32 id) {
  scene_gen::Placement at;
  at.position = t.position;
  at.mesh = mesh;
  at.id = id;
  return at;
}
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present())
    return refused(error, "ledges require static ground; rebake for changed terrain");
  const auto& e = *static_cast<Entry*>(state);
  const u32 offset = out.meshes.size();
  for (const auto& m : e.meshes)
    out.meshes.push_back(m);
  for (u32 i = 0; i < e.tiles.size(); ++i)
    if (e.tiles[i].mesh[0] != k_none) {
      out.instances.push_back(placement_of(e.tiles[i], e.tiles[i].mesh[0] + offset, i + 1));
      ++out.things;
    }
  return true;
}
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  for (const auto& m : static_cast<Entry*>(state)->meshes)
    out.push_back(m);
  return true;
}
bool tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  if (c.stand.present())
    return refused(error, "ledges require static ground; rebake for changed terrain");
  const auto& e = *static_cast<Entry*>(state);
  const u8 form = form_of(e, c);
  out.kind = form;
  if (form > 1) return true;
  for (u32 i = 0; i < e.tiles.size(); ++i)
    if (e.tiles[i].tile == tile && e.tiles[i].mesh[form] != k_none) {
      out.instances.push_back(placement_of(e.tiles[i], e.tiles[i].mesh[form], i + 1));
      ++out.things;
    }
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  for (const auto& t : static_cast<const Entry*>(state)->tiles)
    if (t.tile == tile) return true;
  return false;
}
u8 representation(const void* state, const scene_gen::Context& c) noexcept {
  return form_of(*static_cast<const Entry*>(state), c);
}
constexpr scene_gen::PlacementGeneratorDesc k_ledges{.name = k_ledges_generator,
                                                     .open = &open,
                                                     .close = &close,
                                                     .expand = &expand,
                                                     .meshes = &meshes,
                                                     .tile = &tile,
                                                     .occupies = &occupies,
                                                     .representation = &representation};
const scene_gen::Registrar k_registrar{k_ledges};
}  // namespace
}  // namespace engine::rocks
