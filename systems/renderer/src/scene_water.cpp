// A scene's water bodies (scene_water.h; docs/subsystems/water.md, "Where it enters the renderer").
//
// The renderer is not a capability and the water is (ADR-0027), so the meshes come from the water
// capability's placement generator, found by name in the registry as every placement generator is
// (ADR-0046), and the renderer names nothing else of it. What is the renderer's is the material: a
// body's shading numbers — its colours with their defaults, its roughness and waves, and the
// direction they travel, the ground's wind where the block follows it — resolved here, where the
// ground provider is, and packed by the GPU scene into the mesh's material
// (`gfx::set_water_material`).
#include <core/log/log.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/gfx/water_spectrum.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstring>
#include <renderer_log.h>
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
                             const TerrainSampler& ground, f32& wind) {
  gfx::WaterShading s;
  wind = 0.0f;
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
    wind = len;
  } else {
    const f32 a = radians(body.direction_deg);
    s.direction = Vec2{std::cos(a), std::sin(a)};
  }
  return s;
}

// The body's depth map beside its mesh (systems/water's `depth_map_bytes`, whose layout this
// mirrors because the renderer may not call the capability; water_scene_tests.cpp holds the two
// together): `<stem>.depth` for `<stem>.glb`. No file, or one this reader does not know, is no
// map, and the water branch draws from the vertex depths as before the map.
SceneWaterDepth read_depth_map(const std::string& mesh_path) {
  SceneWaterDepth map;
  constexpr std::string_view k_glb = ".glb";
  if (mesh_path.size() <= k_glb.size() ||
      mesh_path.compare(mesh_path.size() - k_glb.size(), k_glb.size(), k_glb) != 0)
    return map;
  const std::string path = mesh_path.substr(0, mesh_path.size() - k_glb.size()) + ".depth";
  std::string bytes;
  if (!io::exists(path) || io::read_file(path, bytes) != io::Status::Ok) return map;
  const auto byte = [&](usize at) { return u32{static_cast<u8>(bytes[at])}; };
  const auto u32_at = [&](usize at) {
    return byte(at) | (byte(at + 1) << 8) | (byte(at + 2) << 16) | (byte(at + 3) << 24);
  };
  const auto f32_at = [&](usize at) {
    const u32 bits = u32_at(at);
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  };
  // "EWDM", version 1, the size, 0, origin x and z, the texel, the range; then u16 texels.
  if (bytes.size() < 32 || u32_at(0) != 0x4d445745u || u32_at(4) != 1u) {
    ENGINE_LOG_WARN(log_renderer, "water depth map unread", log::field("path", path));
    return map;
  }
  const u32 size = u32_at(8);
  if (size < 2 || size > 4096 || bytes.size() != 32 + usize{size} * size * 2) {
    ENGINE_LOG_WARN(log_renderer, "water depth map unread", log::field("path", path));
    return map;
  }
  map.size = size;
  map.origin = Vec2{f32_at(16), f32_at(20)};
  map.texel_m = f32_at(24);
  map.depth_max = f32_at(28);
  map.texels.resize(size * size);
  for (u32 t = 0; t < size * size; ++t)
    map.texels[t] = static_cast<u16>(byte(32 + usize{t} * 2) | (byte(33 + usize{t} * 2) << 8));
  return map;
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
    SceneWater water;
    water.body = file.water[k];
    f32 wind = 0.0f;
    water.shading = shading_of(file.water[k], out.terrain, ground, wind);
    // An open sea's waves from the spectrum (water.md, "The spectrum"): the ground's wind where it
    // has one and the body follows it, the body's own otherwise; none, and the four sines.
    if (wind <= 0.0f) wind = file.water[k].wind_m_s;
    if (file.water[k].open && wind > 0.0f) {
      gfx::WaterWave waves[gfx::k_water_waves];
      gfx::water_spectrum(static_cast<f64>(wind),
                          static_cast<f64>(file.water[k].fetch_km) * 1000.0,
                          water.shading.direction, file.water[k].seed, waves);
      for (const gfx::WaterWave& wave : waves)
        water.waves.push_back(wave);
    }
    water.depth = read_depth_map(placed.meshes[k].path);
    info.water = std::move(water);
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
