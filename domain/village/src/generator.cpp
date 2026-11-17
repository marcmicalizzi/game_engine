// The placement generator "village" (docs/subsystems/village.md, "In a scene"): the plan made when
// the entry is opened, on the ground before any edit, and handed over three ways — its treads, pads
// and paths to the ground (`plan_ground`), the same as shapes to the entries after it, and a house
// a built plot as a placement whose mesh is a recipe the tile hands.
#include "detail.h"

#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace engine::village {

ENGINE_LOG_CATEGORY_DEFINE(log_village, "village");

namespace {

// How far past a tile a shape is still handed with it: what a later entry may keep clear by (the
// routes hand theirs within their wear's 2 m).
constexpr i64 k_shape_reach_mm = 2000;

struct TileKey {
  i64 z = 0;
  i64 x = 0;
  constexpr bool operator<(const TileKey& o) const noexcept { return z != o.z ? z < o.z : x < o.x; }
  constexpr bool operator==(const TileKey&) const = default;
};

struct Entry {
  VillageLayer layer;
  VillagePlan plan;
  detail::HouseKit kit;
  // Each kit row's mesh, resolved against the scene's directory.
  Vector<std::string> kit_paths;
  i64 tile_mm = 32000;
  // The tiles a shape or a house reaches, sorted and unique: what `occupies` answers from.
  Vector<TileKey> tiles;
};

bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
constexpr const char* k_moving =
    "a village requires static ground; a moving ground keeps no pad and no lane";

constexpr i64 floor_div(i64 a, i64 b) noexcept { return a / b - ((a % b != 0 && (a < 0)) ? 1 : 0); }

TileKey tile_of(const Entry& e, i64 x_mm, i64 z_mm) noexcept {
  return TileKey{floor_div(z_mm, e.tile_mm), floor_div(x_mm, e.tile_mm)};
}

void file_tiles(Entry& e) {
  e.tiles.clear();
  scene_gen::FootprintList all;
  footprints(e.layer, e.plan, all);
  for (u32 k = 0; k < all.size(); ++k) {
    const scene_gen::Footprint& f = all[k];
    const i64 r = static_cast<i64>(f.radius * 1000.0) + 1 + k_shape_reach_mm;
    const TileKey lo = tile_of(e, f.x_mm - r, f.z_mm - r), hi = tile_of(e, f.x_mm + r, f.z_mm + r);
    for (i64 z = lo.z; z <= hi.z; ++z)
      for (i64 x = lo.x; x <= hi.x; ++x)
        e.tiles.push_back(TileKey{z, x});
  }
  for (const Plot& p : e.plan.plots)
    if (p.state == PlotState::built) e.tiles.push_back(tile_of(e, p.house_x_mm, p.house_z_mm));
  std::sort(e.tiles.begin(), e.tiles.end());
  u32 kept = 0;
  for (u32 k = 0; k < e.tiles.size(); ++k)
    if (kept == 0 || !(e.tiles[k] == e.tiles[kept - 1])) e.tiles[kept++] = e.tiles[k];
  e.tiles.resize(kept);
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  if (!schema::from_json(e->layer, params, reader) || !reader.ok()) {
    std::string message = "engine.village.VillageLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  if (context.stand.present()) return refused(error, k_moving);
  // **The plan stands on the ground before any edit** (scene_gen.md, "What an entry plans for the
  // ground"): the open that hands the pads over and the open on the padded ground read the same
  // heights and make one plan.
  // **The kit** (village.md, "A kit"): its rows as the furnisher reads them, and their meshes as
  // the host resolves a scene's (relative to the scene file's directory).
  for (u32 r = 0; r < e->layer.kit.size(); ++r) {
    const std::string& mesh = e->layer.kit[r].mesh;
    if (mesh.empty()) return refused(error, "kit row " + std::to_string(r) + " names no mesh");
    if (!(e->layer.kit[r].scale > 0.0f))
      return refused(error, "kit row " + std::to_string(r) + ": scale must be above 0");
    const bool absolute = mesh[0] == '/' || mesh[0] == '\\' || (mesh.size() > 1 && mesh[1] == ':');
    e->kit_paths.push_back(absolute || context.dir.empty() ? mesh
                                                           : io::join_path(context.dir, mesh));
  }
  detail::house_kit(e->layer, e->kit);
  void* house_context = nullptr;
  const HouseFn house = detail::house_filler(e->kit, house_context);
  std::string why;
  if (!plan_village(e->layer, context.uncut(), context, house, house_context, e->plan, why))
    return refused(error, why);
  // **Houses asked against houses built** (village.md, "Houses asked and built"; need 133), said
  // at the cook: a warning when fewer stand than the entry asked, with what took the rest.
  const VillagePlan& vp = e->plan;
  if (vp.built < e->layer.houses) {
    ENGINE_LOG_WARN(log_village, "fewer houses built than asked",
                    log::field("entry", e->layer.name.c_str()),
                    log::field("asked", e->layer.houses), log::field("built", vp.built),
                    log::field("plots", static_cast<u32>(vp.plots.size())),
                    log::field("refused", vp.refused), log::field("no_fit", vp.no_fit),
                    log::field("steep", vp.steep), log::field("wet", vp.wet),
                    log::field("outside", vp.outside),
                    log::field("first_refusal", vp.first_refusal.c_str()));
  } else {
    ENGINE_LOG_INFO(log_village, "houses built as asked",
                    log::field("entry", e->layer.name.c_str()),
                    log::field("asked", e->layer.houses), log::field("built", vp.built),
                    log::field("cottages", vp.cottages));
  }
  e->tile_mm = static_cast<i64>(static_cast<f64>(context.tile_size) * 1000.0 + 0.5);
  if (e->tile_mm < 1) e->tile_mm = 1;
  file_tiles(*e);
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }

bool plan_ground(void* state, const scene_gen::Context& c, scene_gen::GroundPlan& out,
                 std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const Entry& e = *static_cast<const Entry*>(state);
  ground_edits(e.layer, e.plan, out.edits);
  return true;
}

// The houses of the plan whose origin `wanted` takes, as placements over `out.meshes`: one mesh a
// distinct recipe (two houses of one description share it), named by the recipe's own path.
// A house's kit pieces follow it (village.md, "A kit"): one mesh a kit row the tile names, each
// piece a placement composed with its house's.
template <class Wanted>
u32 place_houses(const Entry& e, scene_gen::Placements& out, const Wanted& wanted) {
  u32 placed = 0;
  for (const Plot& p : e.plan.plots) {
    if (p.state != PlotState::built) continue;
    const House& h = e.plan.houses[p.house];
    ++out.things;
    if (h.recipe.empty() || !wanted(p)) continue;  // a stand-in box has no mesh
    u32 mesh = out.meshes.size();
    for (u32 k = 0; k < out.meshes.size(); ++k)
      if (out.meshes[k].path == h.path) mesh = k;
    if (mesh == out.meshes.size())
      out.meshes.push_back(scene_gen::PlacementMesh(h.path, h.name, 0, h.recipe, h.reach));
    scene_gen::Placement placement = house_placement(p);
    placement.mesh = mesh;
    placement.id = p.house + 1;
    out.instances.push_back(placement);
    ++placed;
    for (u32 j = 0; j < h.kit.size(); ++j) {
      const KitPlace& k = h.kit[j];
      if (k.row >= e.kit_paths.size()) continue;
      const std::string& path = e.kit_paths[k.row];
      u32 kit_mesh = out.meshes.size();
      for (u32 m = 0; m < out.meshes.size(); ++m)
        if (out.meshes[m].recipe.empty() && out.meshes[m].path == path) kit_mesh = m;
      if (kit_mesh == out.meshes.size())
        out.meshes.push_back(scene_gen::PlacementMesh(path, "kit/" + e.layer.kit[k.row].name));
      scene_gen::Placement piece = kit_placement(e.layer, p, k);
      piece.mesh = kit_mesh;
      // After every house's id: the house's and the piece's place in it.
      piece.id = 0x01000000u + p.house * 256u + j;
      out.instances.push_back(piece);
    }
  }
  return placed;
}

bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const Entry& e = *static_cast<const Entry*>(state);
  place_houses(e, out, [](const Plot&) { return true; });
  footprints(e.layer, e.plan, out.shapes);
  return true;
}
bool meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>&, std::string*) {
  return true;  // nothing is resident before its tile: a house's mesh arrives with its tile
}
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const Entry& e = *static_cast<const Entry*>(state);
  const TileKey key{at.z, at.x};
  out.things = place_houses(
      e, out, [&](const Plot& p) { return tile_of(e, p.house_x_mm, p.house_z_mm) == key; });
  footprints(e.layer, e.plan, out.shapes, e.tile_mm, at.x, at.z, k_shape_reach_mm);
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord at) noexcept {
  const Entry& e = *static_cast<const Entry*>(state);
  return std::binary_search(e.tiles.begin(), e.tiles.end(), TileKey{at.z, at.x});
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }

constexpr scene_gen::PlacementGeneratorDesc k_village{.name = "village",
                                                      .open = &open,
                                                      .close = &close,
                                                      .expand = &expand,
                                                      .meshes = &meshes,
                                                      .tile = &tile,
                                                      .occupies = &occupies,
                                                      .representation = &representation,
                                                      .tile_meshes = true,
                                                      .tile_meshes_masked = false,
                                                      .plan_ground = &plan_ground};
const scene_gen::Registrar k_registrar{k_village};

}  // namespace
}  // namespace engine::village
