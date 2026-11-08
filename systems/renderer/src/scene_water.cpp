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
#include <domain/gfx/water_spectrum.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <algorithm>
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
      gfx::water_spectrum(static_cast<f64>(wind), static_cast<f64>(file.water[k].fetch_km) * 1000.0,
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

// **Channels** (scene_water.h; docs/subsystems/water.md, "Channels"; roadmap S96).
namespace {

// A channel's optics as a body carries them, so the GPU scene packs its material, its volume
// optics and its depth map exactly as it does a body's: the same names, the same defaults
// (engine.scene.WaterChannel). The body has no shape, which `water::read_body` refuses, so a host
// that wades in the scene's bodies (engine-view's walker) leaves a channel out.
scene::WaterBody channel_body(const scene::WaterChannel& channel) {
  scene::WaterBody body;
  body.name = channel.name;
  body.level_m = channel.points.empty() ? 0.0 : channel.points[0].y;
  body.shore_m = channel.shore_m;
  body.shallow = channel.shallow;
  body.deep = channel.deep;
  body.depth_fade_m = channel.depth_fade_m;
  body.roughness = channel.roughness;
  body.seed = static_cast<u32>(channel.seed ^ (channel.seed >> 32));
  body.absorption = channel.absorption;
  body.scattering = channel.scattering;
  body.transmission = channel.transmission;
  body.turbidity = channel.turbidity;
  return body;
}

// **The plunges** (water.md, "The plunge"; roadmap S98). A stretch falls where it drops at least
// as far as it runs (systems/water's `stretch_falls`, k_channel_fall_gradient 1, mirrored here
// because the renderer may not call the capability); a run of falling stretches is one fall from
// the first one's lip to the last one's foot. A foot that lies inside a body — a texel of the
// body's depth map under it holds water — and within a metre of the body's level lands in it: the
// body draws its white water (`SceneWater::plunges`), and a channel whose last point is such a foot
// is "pooled", so the ribbon builds no plunge stretch over the pool (channel.h).
constexpr f64 k_plunge_level_m = 1.0;
constexpr f64 k_plunge_radius_share = 0.25;  // the disc's radius: the fall's height over four
constexpr f64 k_plunge_radius_min_m = 1.0;
constexpr f64 k_plunge_radius_max_m = 8.0;

bool stretch_falls_here(const scene::WaterChannel& c, u32 i) {
  if (i + 1 >= c.points.size()) return false;
  const f64 dx = c.points[i + 1].x - c.points[i].x;
  const f64 dz = c.points[i + 1].z - c.points[i].z;
  const f64 drop = c.points[i].y - c.points[i + 1].y;
  return drop > 0.0 && drop >= std::sqrt(dx * dx + dz * dz);
}

// Whether body-local (x, z) is over water in the body's map (its nearest texel).
bool over_water(const SceneWaterDepth& map, f64 x, f64 z) {
  if (map.size < 2 || !(map.texel_m > 0.0f)) return false;
  const f64 i =
      std::floor((x - static_cast<f64>(map.origin.x)) / static_cast<f64>(map.texel_m) + 0.5);
  const f64 j =
      std::floor((z - static_cast<f64>(map.origin.y)) / static_cast<f64>(map.texel_m) + 0.5);
  if (i < 0.0 || j < 0.0 || i >= static_cast<f64>(map.size) || j >= static_cast<f64>(map.size))
    return false;
  return map.texels[static_cast<u32>(j) * map.size + static_cast<u32>(i)] > 0;
}

// Finds the bodies the channels' falls land in, fills their plunges, and returns the channels
// whose last point lands in one.
Vector<u32> find_plunges(const scene::Scene& file, SceneDesc& out) {
  Vector<u32> pooled;
  // The bodies already read: mesh index and the instance that places it.
  struct BodyAt {
    u32 mesh;
    WorldPos origin;
  };
  Vector<BodyAt> bodies;
  for (const SceneInstance& instance : out.instances) {
    if (instance.mesh >= out.mesh_info.size()) continue;
    const SceneMeshInfo& info = out.mesh_info[instance.mesh];
    if (!info.water.has_value() || info.water->channel || info.water->depth.size < 2) continue;
    bodies.push_back(BodyAt{instance.mesh, instance.origin});
  }
  if (bodies.empty()) return pooled;
  for (u32 k = 0; k < file.water_channels.size(); ++k) {
    const scene::WaterChannel& c = file.water_channels[k];
    const u32 n = c.points.size();
    for (u32 i = 0; i + 1 < n; ++i) {
      // The foot of a fall: the stretch into point i + 1 falls and the one out of it does not.
      if (!stretch_falls_here(c, i) || stretch_falls_here(c, i + 1)) continue;
      u32 top = i;
      while (top > 0 && stretch_falls_here(c, top - 1))
        --top;
      const WorldPos& foot = c.points[i + 1];
      const f64 height = c.points[top].y - foot.y;
      for (const BodyAt& body : bodies) {
        SceneWater& water = *out.mesh_info[body.mesh].water;
        if (std::abs(foot.y - water.body.level_m) > k_plunge_level_m) continue;
        const f64 x = foot.x - body.origin.x;
        const f64 z = foot.z - body.origin.z;
        if (!over_water(water.depth, x, z)) continue;
        gfx::WaterPlunge plunge;
        plunge.at = Vec2{static_cast<f32>(x), static_cast<f32>(z)};
        plunge.radius_m = static_cast<f32>(std::clamp(
            k_plunge_radius_share * height, k_plunge_radius_min_m, k_plunge_radius_max_m));
        plunge.speed_m_s = static_cast<f32>(std::sqrt(2.0 * 9.81 * height));
        water.plunges.push_back(plunge);
        if (i + 2 == n) pooled.push_back(k);
        break;
      }
    }
  }
  // The highest falls first, and no more than a table entry holds.
  for (const BodyAt& body : bodies) {
    Vector<gfx::WaterPlunge>& list = out.mesh_info[body.mesh].water->plunges;
    if (list.empty()) continue;
    std::stable_sort(list.begin(), list.end(),
                     [](const gfx::WaterPlunge& a, const gfx::WaterPlunge& b) {
                       return a.radius_m > b.radius_m;
                     });
    if (list.size() > gfx::k_water_plunges) {
      ENGINE_LOG_WARN(log_renderer,
                      "a water body has more plunges than it keeps; the lowest dropped",
                      log::field("body", out.mesh_info[body.mesh].name),
                      log::field("plunges", list.size()), log::field("kept", gfx::k_water_plunges));
      list.resize(gfx::k_water_plunges);
    }
  }
  return pooled;
}

}  // namespace

bool read_scene_water_channels(const scene::Scene& file, const std::string& path,
                               const std::string& dir, const TerrainSampler& ground, SceneDesc& out,
                               std::string& error) {
  if (file.water_channels.empty()) return true;
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement("water_channel");
  if (generator == nullptr) {
    error = path +
            ": the scene has water channels, and this build carries no water capability "
            "(ENGINE_WITH_WATER is off): " +
            registry.unknown_placement("water_channel");
    return false;
  }
  scene_gen::Context context;
  context.world_seed = out.terrain.enabled ? out.terrain.seed : 0;
  context.dir = dir;
  const std::string where = path + ": water_channels";
  context.where = where;
  context.ddc = out.ddc;
  if (out.terrain.enabled) context.ground = ground.provider().view();
  JsonValue list = JsonValue::array();
  for (const scene::WaterChannel& channel : file.water_channels)
    list.push_back(schema::to_json(channel));
  JsonValue params = JsonValue::object();
  params.set("channels", std::move(list));
  // The channels whose falls land in a body (S98): the body draws the plunge, and a channel that
  // ends in one builds no plunge stretch over the pool.
  const Vector<u32> pooled = find_plunges(file, out);
  if (!pooled.empty()) {
    JsonValue pooled_list = JsonValue::array();
    for (const u32 k : pooled)
      pooled_list.push_back(JsonValue(u64{k}));
    params.set("pooled", std::move(pooled_list));
  }
  void* state = nullptr;
  if (!generator->open(params, context, &state, &error)) return false;
  struct Close {
    const scene_gen::PlacementGeneratorDesc* generator;
    void* state;
    ~Close() { generator->close(state); }
  } close{generator, state};
  scene_gen::Placements placed;
  if (!generator->expand(state, context, placed, &error)) return false;
  const u32 count = file.water_channels.size();
  if (placed.meshes.size() != count || placed.instances.size() != count) {
    error = where + ": the water channel generator made " + std::to_string(placed.meshes.size()) +
            " meshes for " + std::to_string(count) + " channels";
    return false;
  }
  for (u32 k = 0; k < count; ++k) {
    const u32 mesh = out.meshes.size();
    out.meshes.push_back(placed.meshes[k].path);
    SceneMeshInfo info;
    info.name = placed.meshes[k].name;
    info.origin = "water";
    SceneWater water;
    water.body = channel_body(file.water_channels[k]);
    const scene::WaterChannel& c = file.water_channels[k];
    if (c.shallow.has_value()) water.shading.shallow = *c.shallow;
    if (c.deep.has_value()) water.shading.deep = *c.deep;
    water.shading.roughness = c.roughness;
    water.shading.depth_fade_m = c.depth_fade_m;
    water.shading.shore_m = c.shore_m;
    water.shading.seed = water.body.seed;
    water.channel = true;
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
