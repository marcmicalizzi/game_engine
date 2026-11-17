// A scene's water bodies (scene_water.h; docs/subsystems/water.md, "Why a capability").
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
#include <domain/gfx/water_swash.h>
#include <domain/gfx/wet_ground.h>
#include <domain/scene_gen/derived_root.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/water_channel.h>
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
  // day does not turn the waves until the scene is read again (water.md, "The block").
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
  // "EWDM", the version, the size, the bank (version 1: 0), origin x and z, the texel, the
  // deepest, and since version 3 the mesh's three spacings; then u16 texels over [-bank, deepest]
  // (version 1: [0, deepest]).
  const u32 version = bytes.size() >= 8 ? u32_at(4) : 0u;
  if (bytes.size() < 32 || u32_at(0) != 0x4d445745u || version < 1u || version > 3u) {
    ENGINE_LOG_WARN(log_renderer, "water depth map unread", log::field("path", path));
    return map;
  }
  const usize head = version >= 3u ? 44 : 32;
  const u32 size = u32_at(8);
  if (size < 2 || size > 4096 || bytes.size() != head + usize{size} * size * 2) {
    ENGINE_LOG_WARN(log_renderer, "water depth map unread", log::field("path", path));
    return map;
  }
  map.size = size;
  map.origin = Vec2{f32_at(16), f32_at(20)};
  map.texel_m = f32_at(24);
  map.depth_max = f32_at(28);
  map.bank_max = version == 1u ? 0.0f : f32_at(12);
  map.mesh_far_m = version >= 3u ? f32_at(32) : 4.0f * map.texel_m;
  map.mesh_near_m = version >= 3u ? f32_at(36) : 4.0f * map.texel_m;
  map.mesh_grade = version >= 3u ? f32_at(40) : 0.0f;
  if (!(map.mesh_far_m >= 0.0f) || !(map.mesh_near_m >= 0.0f) || !(map.mesh_grade >= 0.0f) ||
      !std::isfinite(map.mesh_far_m + map.mesh_near_m + map.mesh_grade)) {
    ENGINE_LOG_WARN(log_renderer, "water depth map unread", log::field("path", path));
    return SceneWaterDepth{};
  }
  if (!(map.bank_max >= 0.0f) || !std::isfinite(map.bank_max)) {
    ENGINE_LOG_WARN(log_renderer, "water depth map unread", log::field("path", path));
    return SceneWaterDepth{};
  }
  map.texels.resize(size * size);
  for (u32 t = 0; t < size * size; ++t)
    map.texels[t] =
        static_cast<u16>(byte(head + usize{t} * 2) | (byte(head + 1 + usize{t} * 2) << 8));
  return map;
}

// **A closed body's fetch** (water.md, "The spectrum"; S100 stage B): the longest reach of its own
// water along its wind — the fetch field's largest value over the depth map — no longer than the
// block's `fetch_km`: a wind cannot blow over more water than the lake holds along it. With no map,
// the block's.
f64 closed_fetch_m(const SceneWaterDepth& map, Vec2 wind, f64 fetch_m) {
  if (map.size < 2) return fetch_m;
  const u32 count = map.size * map.size;
  Vector<f32> depth(count);
  const f64 range = static_cast<f64>(map.depth_max) + static_cast<f64>(map.bank_max);
  for (u32 i = 0; i < count; ++i)
    depth[i] = static_cast<f32>(static_cast<f64>(map.texels[i]) / 65535.0 * range -
                                static_cast<f64>(map.bank_max));
  Vector<f32> fetch(count);
  const f64 longest = gfx::water_fetch_field(depth.data(), map.size, static_cast<f64>(map.texel_m),
                                             wind, fetch.data());
  return std::min(fetch_m, longest);
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
  // The host's derived-data root, set on the description before the file is read, else the
  // scene's own: the one rule (T119; scene_gen.md, "The derived-data root"). The pool's mesh is
  // written under it, never under a `ddc/` found from the working directory.
  const std::string root = scene_gen::host_derived_root(out.ddc, dir, true);
  context.ddc = root;
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
    // **Every body's waves from the spectrum** (water.md, "The spectrum"; S100 stage B): an open
    // body's wind is the ground's where it has one and the body follows it, its own otherwise, over
    // its `fetch_km`; a closed body's is its own `wind_m_s` (a pool under trees does not feel the
    // region's wind unless its block says so) over its own reach along it. No wind: the four sines.
    if (wind <= 0.0f || !file.water[k].open) wind = file.water[k].wind_m_s;
    water.wind_m_s = wind;
    water.depth = read_depth_map(placed.meshes[k].path);
    water.fetch_m = file.water[k].fetch_km * 1000.0f;
    if (!file.water[k].open && wind > 0.0f)
      water.fetch_m = static_cast<f32>(
          closed_fetch_m(water.depth, water.shading.direction, static_cast<f64>(water.fetch_m)));
    if (wind > 0.0f && water.fetch_m > 0.0f) {
      gfx::WaterWave waves[gfx::k_water_waves];
      gfx::water_spectrum(static_cast<f64>(wind), static_cast<f64>(water.fetch_m),
                          water.shading.direction, file.water[k].seed, waves);
      for (const gfx::WaterWave& wave : waves)
        water.waves.push_back(wave);
    }
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

// **The plunges** (water.md, "The plunge"; roadmap S98, S106). A stretch falls where the solve
// says so: a station flagged a fall's lip and the next its foot (`scene_gen/water_channel.h`; the
// description is checked before this reader sees it). A foot that lies inside a body — a texel of
// the body's depth map under it holds water — and within a metre of the body's level lands in it,
// and the body draws its white water (`SceneWater::plunges`).
constexpr f64 k_plunge_level_m = 1.0;
constexpr f64 k_plunge_radius_share = 0.25;  // the disc's radius: the fall's height over four
constexpr f64 k_plunge_radius_min_m = 1.0;
constexpr f64 k_plunge_radius_max_m = 8.0;

bool stretch_falls_here(const scene::WaterChannel& c, u32 i) {
  if (i + 1 >= c.points.size() || i + 1 >= c.flags.size()) return false;
  return (c.flags[i] & scene_gen::k_channel_fall_lip) != 0 &&
         (c.flags[i + 1] & scene_gen::k_channel_fall_foot) != 0 &&
         c.points[i].y > c.points[i + 1].y;
}

// **A channel's banks** (water.md, "Channels"; roadmap S99, S106): one `gfx::WaterShore` stretch
// record a drawn stretch — one whose two stations do not both stand over a bank, as the sheet's
// are — its two points the middles of the solved wet reach at its stations, relative to the
// channel instance's place `at`, its half widths half the wet reach, and its fringe — the
// capillary rise of the bank's material (`gfx::capillary_fringe_m`; a channel's description names
// no material, so `gfx::k_bank_default`) plus the spray band of its regime: a fall
// (`stretch_falls_here`) its share of the drop, a rapid (the solved Froude number above 1 at
// either end) a fixed band. Static: no swash, `material` none.
Vector<gfx::WaterShore> channel_bank_records(const scene::WaterChannel& c, const WorldPos& at) {
  Vector<gfx::WaterShore> out;
  const f32 fringe = gfx::capillary_fringe_m(gfx::k_bank_default);
  const u32 n = c.points.size();
  if (c.wet_left_m.size() != n || c.wet_right_m.size() != n || c.froude.size() != n ||
      c.flags.size() != n)
    return out;
  for (u32 i = 0; i + 1 < n; ++i) {
    const WorldPos& p = c.points[i];
    const WorldPos& q = c.points[i + 1];
    // A stretch with no sheet has no banks: both stations over a bank, and the water written
    // there a film or climbing (the water capability's `stretch_drawn`, channel.h, whose two
    // numbers these are: a film under 2 cm, a rise over a millimetre; S108).
    if ((c.flags[i] & c.flags[i + 1] & scene_gen::k_channel_over_bank) != 0 &&
        (c.depth_m.size() != n || c.depth_m[i] < 0.02f || c.depth_m[i + 1] < 0.02f ||
         q.y > p.y + 0.001))
      continue;
    f32 spray = 0.0f;
    if (stretch_falls_here(c, i)) {
      const f64 drop = p.y - q.y;
      spray = std::min(gfx::k_spray_fall_share * static_cast<f32>(drop), gfx::k_spray_fall_max_m);
    } else if (c.froude[i] > 1.0f || c.froude[i + 1] > 1.0f) {
      spray = gfx::k_spray_rapid_m;
    }
    // Left of the line is the side of (-dz, dx): the wet reach's middle is half the difference of
    // its two sides that way.
    const f64 dx = q.x - p.x, dz = q.z - p.z;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    const f64 lx = len > 0.0 ? -dz / len : 0.0, lz = len > 0.0 ? dx / len : 0.0;
    const auto middle = [&](const WorldPos& s, u32 k) {
      const f64 shift =
          0.5 * (static_cast<f64>(c.wet_left_m[k]) - static_cast<f64>(c.wet_right_m[k]));
      return Vec4{static_cast<f32>(s.x + lx * shift - at.x),
                  static_cast<f32>(s.z + lz * shift - at.z), static_cast<f32>(s.y - at.y),
                  std::max(0.5f * (c.wet_left_m[k] + c.wet_right_m[k]), 0.01f)};
    };
    gfx::WaterShore r;
    r.wet_m = fringe + spray;
    r.material = 0xffffffffu;
    r.stretch_a = middle(p, i);
    r.stretch_b = middle(q, i + 1);
    // The stretch's square, grown by its half width and where a 1:25 bank rises the fringe: the
    // run alone dries the ground there (water.slang's `water_channel_saturation`), so nothing past
    // it can be wet and the square ends at it.
    const f32 grow = std::max(r.stretch_a.w, r.stretch_b.w) + 25.0f * r.wet_m;
    r.lo = Vec2{std::min(r.stretch_a.x, r.stretch_b.x) - grow,
                std::min(r.stretch_a.y, r.stretch_b.y) - grow};
    r.hi = Vec2{std::max(r.stretch_a.x, r.stretch_b.x) + grow,
                std::max(r.stretch_a.y, r.stretch_b.y) + grow};
    out.push_back(r);
  }
  return out;
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

// Finds the bodies the channels' falls land in and fills their plunges.
void find_plunges(const scene::Scene& file, SceneDesc& out) {
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
  if (bodies.empty()) return;
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
        break;
      }
    }
  }
  // The highest falls first. **A plunge whose centre lies in a higher one's disc is that one's
  // white water** (W138): the falls of one cascade, or of two lines, landing within a few metres
  // are one churned patch, and drawing two discs over it draws nothing more. Until W138 each was a
  // record of its own, and at twice the forest's flow three bodies held more than a table entry
  // keeps and the load warned of each. Then no more than a table entry holds: what is left past it
  // is the lowest of falls that stand apart, said once at information level.
  for (const BodyAt& body : bodies) {
    Vector<gfx::WaterPlunge>& list = out.mesh_info[body.mesh].water->plunges;
    if (list.empty()) continue;
    std::stable_sort(list.begin(), list.end(),
                     [](const gfx::WaterPlunge& a, const gfx::WaterPlunge& b) {
                       return a.radius_m > b.radius_m;
                     });
    const u32 found = list.size();
    u32 kept = 0;
    for (u32 k = 0; k < list.size(); ++k) {
      bool inside = false;
      for (u32 j = 0; j < kept && !inside; ++j) {
        const f32 dx = list[k].at.x - list[j].at.x, dz = list[k].at.y - list[j].at.y;
        inside = dx * dx + dz * dz < list[j].radius_m * list[j].radius_m;
      }
      if (!inside) list[kept++] = list[k];
    }
    list.resize(kept);
    if (list.size() > gfx::k_water_plunges) {
      ENGINE_LOG_INFO(log_renderer, "a water body has more falls apart than it keeps; the lowest left",
                      log::field("body", out.mesh_info[body.mesh].name),
                      log::field("falls", found), log::field("apart", list.size()),
                      log::field("kept", gfx::k_water_plunges));
      list.resize(gfx::k_water_plunges);
    }
  }
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
  const std::string root = scene_gen::host_derived_root(out.ddc, dir, true);  // T119's one rule
  context.ddc = root;
  if (out.terrain.enabled) context.ground = ground.provider().view();
  JsonValue list = JsonValue::array();
  // A description is solved or it is refused by name, and the refusal is the water capability's
  // (`water::read_channel`, through `scene_gen::water_channel_solved`): the generator's `open`
  // below reads every channel and names the one it refuses.
  for (const scene::WaterChannel& channel : file.water_channels)
    list.push_back(schema::to_json(channel));
  JsonValue params = JsonValue::object();
  params.set("channels", std::move(list));
  // The channels whose falls land in a body (S98): the body draws the plunge. (Until S106 a
  // channel that ended in one was named to the generator so its ribbon built no plunge stretch;
  // the solved description has the water below a foot, and the sheet builds nothing of its own.)
  find_plunges(file, out);
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
    // A channel with no drawn stretch has no sheet (S106: every station over its bank), which the
    // generator says with a mesh of no path: no mesh, no instance and no banks.
    if (placed.meshes[k].path.empty()) continue;
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
    water.channel_source = c;  // A43: what a game module's `water_at` answers from
    water.depth = read_depth_map(placed.meshes[k].path);
    SceneInstance instance;
    instance.mesh = mesh;
    instance.transform = placed.instances[k].turn();
    instance.origin = placed.instances[k].position;
    // The banks (S99), local to where the instance stands: its origin plus its offset, unrotated.
    const WorldPos stands{instance.origin.x + static_cast<f64>(instance.transform.position.x),
                          instance.origin.y + static_cast<f64>(instance.transform.position.y),
                          instance.origin.z + static_cast<f64>(instance.transform.position.z)};
    water.banks = channel_bank_records(c, stands);
    info.water = std::move(water);
    out.mesh_info.push_back(std::move(info));
    out.instances.push_back(instance);
  }
  return true;
}

}  // namespace engine::renderer
