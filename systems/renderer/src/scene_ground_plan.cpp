#include "scene_ground_plan.h"

#include <core/profiling/startup_trace.h>  // stages only: timing, never logic
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/site.h>
#include <domain/scene_gen/terrain_holes.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/terrain.h>

#include <utility>

namespace engine::renderer {

namespace {

// G135: false leaves the placement generators' exclusion unbuilt, so a scene places as it did
// before the holes, channels and water were read (scene_gen.md, "Exclusion"): the before of a
// before and after, and the way out should a scene want grass in a bed. Read once, when the file is
// read.
tunables::Bool placement_exclusion{
    "renderer.placement.exclusion", true,
    "Leave a terrain hole, a stream's bed and banks and the water empty of placements "
    "(scene_gen.md, \"Exclusion\"); false places as before G135"};

}  // namespace

Vector<PlacementRef> placement_entries(const scene::Scene& file, const std::string& path) {
  Vector<PlacementRef> entries;
  entries.reserve(file.ruins.size() + file.placements.size());
  for (u32 r = 0; r < file.ruins.size(); ++r)
    entries.push_back(PlacementRef{"ruins", schema::to_json(file.ruins[r]),
                                   path + ": ruins " + std::to_string(r)});
  for (u32 p = 0; p < file.placements.size(); ++p) {
    entries.push_back(PlacementRef{file.placements[p].generator, file.placements[p].params,
                                   path + ": placements " + std::to_string(p),
                                   file.placements[p].stream});
  }
  return entries;
}

bool plan_scene_ground(const scene::Scene& file, const std::string& path, const std::string& dir,
                       std::span<const PlacementRef> entries, bool world, SceneDesc& out,
                       std::string& error) {
  // G135: where the placement generators leave nothing, read from the file once (scene_gen.md,
  // "Exclusion"); a file with no holes, channels or water builds an empty one.
  const Vector<scene::TerrainHole> no_holes;
  const Vector<scene::TerrainHole>& authored =
      file.terrain.has_value() ? file.terrain->holes : no_holes;
  const auto build_exclusion = [&](std::span<const scene::TerrainHole> holes) {
    if (!placement_exclusion.get()) return true;
    if (out.terrain.exclusion.build(holes, {file.water.data(), file.water.size()},
                                    {file.water_channels.data(), file.water_channels.size()},
                                    &error))
      return true;
    error = path + ": " + error;
    return false;
  };
  if (!build_exclusion({authored.data(), authored.size()})) return false;
  // G144: what the ground is like at a point, from the sky's latitude and the water (scene_gen.md,
  // "The site"); read by a placement only where a rule of its asks.
  if (!out.terrain.site.build(file.sky.has_value() ? static_cast<f64>(file.sky->latitude_deg)
                                                   : scene_gen::k_site_default_latitude_deg,
                              {file.water.data(), file.water.size()},
                              {file.water_channels.data(), file.water_channels.size()}, &error)) {
    error = path + ": " + error;
    return false;
  }

  // **What the entries plan for the ground** (scene_gen.md, "What an entry plans for the ground").
  // A scene none of whose generators plans stops here, having asked the registry a name an entry;
  // a name the executable does not carry is the expansion's to refuse, with its sentence.
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  bool any = false;
  for (const PlacementRef& entry : entries) {
    const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement(entry.generator);
    any = any || (generator != nullptr && generator->plan_ground != nullptr);
  }
  if (!any) return true;
  ENGINE_STARTUP_STAGE("ground plans");
  // **The ground as it was before any edit**: the terrain entry less its tile set's edits, the
  // scene's own included, which is the ground `Context::uncut()` hands every later call. So a plan
  // is a function of the entry, the seed and this ground: no entry plans on another's edits, and
  // the entries' order moves no plan.
  TerrainDesc unedited_terrain = out.terrain;
  if (unedited_terrain.tiles.has_value()) unedited_terrain.tiles->edits.clear();
  const TerrainSampler unedited(unedited_terrain);
  if (out.terrain.enabled && !unedited.ok()) {
    error = path + ": " + unedited.error();
    return false;
  }
  const scene::WorldRings rings = file.world.has_value() ? *file.world : scene::WorldRings{};
  if (world && !(rings.tile_size > 0.0f)) {
    error = path + ": the world's tile_size must be positive";
    return false;
  }
  LazyPool pool;
  scene_gen::Context context;
  context.world_seed = out.terrain.enabled ? out.terrain.seed : 0;
  context.tile_size = world ? rings.tile_size : 32.0f;
  context.dir = dir;
  context.ddc = out.ddc;
  if (out.terrain.enabled) context.ground = unedited.provider().view();
  if (!out.terrain.exclusion.empty()) context.exclusion = &out.terrain.exclusion;
  context.site = &out.terrain.site;
  if (out.terrain.formation.has_value()) context.formation = &*out.terrain.formation;
  context.jobs_fn = &LazyPool::get;
  context.jobs_context = &pool;
  Vector<scene::TerrainHole> holes;  // the scene's own, then each entry's
  u32 planned_holes = 0;
  for (const PlacementRef& entry : entries) {
    const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement(entry.generator);
    if (generator == nullptr || generator->plan_ground == nullptr) continue;
    ENGINE_STARTUP_STAGE_DETAIL("ground plan", entry.generator);
    context.where = entry.where;
    context.world = world && entry.stream ? &rings : nullptr;
    void* state = nullptr;
    if (!generator->open(entry.params, context, &state, &error)) return false;
    scene_gen::GroundPlan plan;
    const bool planned = generator->plan_ground(state, context, plan, &error);
    generator->close(state);
    if (!planned) return false;
    if (plan.empty()) continue;
    const std::string who =
        entry.where + ": the placement generator \"" + entry.generator + "\" plans ";
    if (!out.terrain.enabled) {
      error = who + "for a ground the scene has not: it names no terrain";
      return false;
    }
    if (!plan.edits.empty()) {
      // An edit is written into a tile set's heights when the set is built (tileset.md); a ground
      // with no set to build has nowhere to take one, and a tread nobody benched is a scene that
      // does not draw what it says.
      if (!out.terrain.tiles.has_value()) {
        error = who +
                "edits of the ground's heights, which a tile set carries: the terrain names no "
                "\"tiles\" block";
        return false;
      }
      for (scene::TerrainEdit& edit : plan.edits)
        out.terrain.tiles->edits.push_back(std::move(edit));
    }
    if (!plan.holes.empty()) {
      // Refused here, where the entry can be named: the same sentence the terrain gives an
      // authored hole it cannot take.
      scene_gen::TerrainHoles probe;
      std::string why;
      if (!probe.build({plan.holes.data(), plan.holes.size()}, &why)) {
        error = who + "an opening the terrain refuses: " + why;
        return false;
      }
      if (planned_holes == 0)
        for (const scene::TerrainHole& hole : authored)
          holes.push_back(hole);
      for (scene::TerrainHole& hole : plan.holes)
        holes.push_back(std::move(hole));
      planned_holes += plan.holes.size();
    }
  }
  if (planned_holes == 0) return true;
  // The openings join the scene's own holes: compiled again for every level's mesh and the
  // collision, and built into the exclusion again so nothing is placed in one.
  if (!out.terrain.holes.build({holes.data(), holes.size()}, &error)) {
    error = path + ": " + error;
    return false;
  }
  return build_exclusion({holes.data(), holes.size()});
}

bool hand_shapes_on(const scene_gen::FootprintList& shapes, const std::string& where,
                    SceneDesc& out, scene_gen::FootprintList& earlier, std::string& error) {
  if (shapes.empty()) return true;
  // Whatever `renderer.placement.exclusion` says: that switch leaves the scene's holes, channels
  // and water unread, and a shape an entry occupies is none of them — and the world's consumer,
  // which cannot read the switch, adds a tile's shapes the same way.
  std::string why;
  if (!out.terrain.exclusion.add_footprints(shapes.span(), &why)) {
    error = where + ": a shape the entry occupies was refused: " + why;
    return false;
  }
  earlier.add(shapes.span());
  return true;
}

}  // namespace engine::renderer
