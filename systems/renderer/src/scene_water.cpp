// A scene's water bodies (scene_water.h; docs/subsystems/water.md, "Where it enters the renderer").
//
// The renderer is not a capability and the water is (ADR-0027), so the meshes come from the water
// capability's placement generator, found by name in the registry as every placement generator is
// (ADR-0046), and the renderer names nothing else of it. What is the renderer's is the material: a
// body's shading numbers — its colours with their defaults, its roughness and waves, and the
// direction they travel, the ground's wind where the block follows it — resolved here, where the
// ground provider is, and packed by the GPU scene into the mesh's material
// (`gfx::set_water_material`).
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>

#include <cmath>
#include <scene_water.h>

namespace engine::renderer {

namespace {

// The water generator's parameters (systems/water's `generator_params`, which this module cannot
// call): the block's bodies under "bodies".
JsonValue water_params(const Vector<scene::WaterBody>& bodies) {
  JsonValue list = JsonValue::array();
  for (const scene::WaterBody& body : bodies)
    list.push_back(schema::to_json(body));
  JsonValue params = JsonValue::object();
  params.set("bodies", std::move(list));
  return params;
}

gfx::WaterShading shading_of(const scene::WaterBody& body, const TerrainDesc& terrain,
                             const TerrainSampler& ground) {
  gfx::WaterShading s;
  if (body.shallow.has_value()) s.shallow = *body.shallow;
  if (body.deep.has_value()) s.deep = *body.deep;
  s.roughness = body.roughness;
  s.depth_fade_m = body.depth_fade_m;
  s.wave_amplitude_m = body.wave_amplitude_m;
  s.wave_length_m = body.wave_length_m;
  s.chop = body.chop;
  s.shore_m = body.shore_m;
  s.seed = body.seed;
  // The waves' direction: the ground's wind at the scene's time when the block follows it and the
  // ground says one, the block's own direction otherwise. The load's: a wind that veers through the
  // day does not turn the waves until the scene is read again (water.md, "Waves").
  f32 wx = 0.0f;
  f32 wz = 0.0f;
  if (body.wind_follow && terrain.enabled && ground.ok() &&
      ground.provider().wind(terrain.time_s, wx, wz) && (wx != 0.0f || wz != 0.0f)) {
    const f32 len = std::sqrt(wx * wx + wz * wz);
    s.direction = Vec2{wx / len, wz / len};
  } else {
    const f32 a = radians(body.direction_deg);
    s.direction = Vec2{std::cos(a), std::sin(a)};
  }
  return s;
}

}  // namespace

bool read_scene_water(const scene::Scene& file, const std::string& path, const std::string& dir,
                      const TerrainSampler& ground, SceneDesc& out, std::string& error) {
  if (file.water.empty()) return true;
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement("water");
  if (generator == nullptr) {
    error = path +
            ": the scene has water, and this build carries no water capability "
            "(ENGINE_WITH_WATER is off): " +
            registry.unknown_placement("water");
    return false;
  }
  scene_gen::Context context;
  context.world_seed = out.terrain.enabled ? out.terrain.seed : 0;
  context.dir = dir;
  const std::string where = path + ": water";
  context.where = where;
  // The host's derived-data root, set on the description before the file is read (scene_gen.md,
  // "The derived-data root"): the pool's mesh is written under it, not under a `ddc/` found from
  // where the scene is.
  context.ddc = out.ddc;
  if (out.terrain.enabled) context.ground = ground.provider().view();
  void* state = nullptr;
  if (!generator->open(water_params(file.water), context, &state, &error)) return false;
  struct Close {
    const scene_gen::PlacementGeneratorDesc* generator;
    void* state;
    ~Close() { generator->close(state); }
  } close{generator, state};
  scene_gen::Placements placed;
  if (!generator->expand(state, context, placed, &error)) return false;
  // Mesh k and instance k are body k (systems/water, `generator_params`).
  if (placed.meshes.size() != file.water.size() || placed.instances.size() != file.water.size()) {
    error = where + ": the water generator made " + std::to_string(placed.meshes.size()) +
            " meshes for " + std::to_string(file.water.size()) + " bodies";
    return false;
  }
  // After the authored meshes and instances, and that order is nothing a host may rely on or need
  // to predict: engine-view's clip loader asks each mesh's `origin` and never opens a generated
  // one as a clip source (docs/subsystems/apps.md, "Mixed scenes"), whatever its index.
  for (u32 k = 0; k < file.water.size(); ++k) {
    const u32 mesh = out.meshes.size();
    out.meshes.push_back(placed.meshes[k].path);
    SceneMeshInfo info;
    info.name = placed.meshes[k].name;
    info.origin = "water";
    info.water = SceneWater{file.water[k], shading_of(file.water[k], out.terrain, ground)};
    out.mesh_info.push_back(std::move(info));
    SceneInstance instance;
    instance.mesh = mesh;
    instance.transform = placed.instances[k].turn();
    instance.origin = placed.instances[k].position;
    out.instances.push_back(instance);
  }
  return true;
}

}  // namespace engine::renderer
