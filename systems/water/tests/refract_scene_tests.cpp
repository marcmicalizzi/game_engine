// What is under the water is seen, on a GPU (docs/subsystems/water.md, "What is under the water";
// roadmap S86): a pool filling a basin of the renderer's ground, drawn through the renderer's
// reader and the resolve's ray-traced water module with the refraction ray on and off
// (`renderer.water.refract`). A pool over bare ground draws the same bytes either way (the ground
// stays the depth map's answer); a log on the bed is seen through the surface, on the water's
// pixels only, and the same frame twice is the same bytes; a log the frustum's bottom edge leaves
// out is still seen, because the submerged caster volume builds it into the frame's structures
// (`renderer.water.refract_volume`), as a caster and never drawn; and the sun that reaches a hit
// under the water is dimmed by Beer–Lambert over its refracted path up to the surface, against
// the double-precision reference (`water_reference.h`'s `submerged_sun`).
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <core/schema/json_reflect.h>
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/flythrough.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/water_refract.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <water_reference.h>  // domain/gfx/tests

using namespace engine;
using namespace engine::renderer;

TEST_CASE("water refract: the submerged volume is the frustum's water pushed out by the spread") {
  // A camera 2 m over a level at -2 m (the eye is the frame's origin), looking level along +z with
  // a 60-degree field of view: the frustum's bottom edge meets the level 3.46 m out.
  const Mat4 view_proj =
      perspective_reversed_z(60.0f * 3.14159265f / 180.0f, 1.5f, 0.1f) *
      look_at(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f});
  const CasterBounds bounds{Vec3{0.0f, 0.0f, 0.0f}, 200.0f};
  CasterVolume volume;
  REQUIRE(fit_water_refract_volume(view_proj, -2.0f, -6.0f, 2.0f, 500.0f, bounds, volume));
  CHECK(volume.plane_count >= 5u);
  const auto inside = [&](Vec3 p) {
    for (u32 i = 0; i < volume.plane_count; ++i)
      if (volume.planes[i].distance(p) < 0.0f) return false;
    return true;
  };
  // Under the frame's bottom edge, a metre nearer than where it meets the level, 3 m down: inside
  // (the spread reaches it), though the frustum does not hold it.
  CHECK(inside(Vec3{0.0f, -5.0f, 2.5f}));
  // Above the level, under the floor, or far out to the side past the spread: outside.
  CHECK(!inside(Vec3{0.0f, -1.0f, 10.0f}));
  CHECK(!inside(Vec3{0.0f, -6.5f, 10.0f}));
  CHECK(!inside(Vec3{-200.0f, -4.0f, 10.0f}));
  // Behind the eye, farther than the spread: outside.
  CHECK(!inside(Vec3{0.0f, -4.0f, -3.0f}));
  // The eye under the level, or a floor not below it: no volume.
  CHECK(!fit_water_refract_volume(view_proj, 1.0f, -6.0f, 2.0f, 500.0f, bounds, volume));
  CHECK(!fit_water_refract_volume(view_proj, -2.0f, -2.0f, 2.0f, 500.0f, bounds, volume));
  // The reach: ln 255 over the clearest channel's extinction.
  CHECK(water_refract_path_m(Vec3{0.5f, 0.1f, 0.2f}) == doctest::Approx(55.413).epsilon(1e-4));
}

TEST_CASE("water refract: the frame record carries the water's fields") {
  FrameStats stats;
  stats.water_reflect = true;
  stats.water_reflect_views = 1;
  stats.water_reflect_level_m = -1.5f;
  stats.water_refract = true;
  stats.water_refract_views = 1;
  stats.water_refract_floor_m = -6.25f;
  const scene::FrameRecord record = frame_record(stats, 0, 3, 0.1);
  std::string text;
  REQUIRE(write_json(schema::to_json(record), text, JsonWriteOptions{.pretty = false}));
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  scene::FrameRecord back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(back, json, ctx));
  REQUIRE(ctx.ok());
  CHECK(back.water_reflect);
  CHECK(back.water_reflect_views == 1u);
  CHECK(back.water_reflect_level_m == -1.5f);
  CHECK(back.water_refract);
  CHECK(back.water_refract_views == 1u);
  CHECK(back.water_refract_floor_m == -6.25f);
  CHECK(text.find("\"water_refract_floor_m\"") != std::string::npos);
}

#if ENGINE_WATER_TESTS_DUNES

namespace {

std::string base64(const std::vector<u8>& bytes) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const u32 a = bytes[i];
    const u32 b = i + 1 < bytes.size() ? bytes[i + 1] : 0u;
    const u32 c = i + 2 < bytes.size() ? bytes[i + 2] : 0u;
    const u32 w = (a << 16) | (b << 8) | c;
    out += k[(w >> 18) & 63];
    out += k[(w >> 12) & 63];
    out += i + 1 < bytes.size() ? k[(w >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? k[w & 63] : '=';
  }
  return out;
}

template <typename T>
void put(std::vector<u8>& out, T value) {
  u8 raw[sizeof(T)];
  std::memcpy(raw, &value, sizeof(T));
  out.insert(out.end(), raw, raw + sizeof(T));
}

// A closed box from (-hx, 0, -hz) to (hx, h, hz) of one colour, as a glTF with its buffer inline.
std::string box_gltf(f32 hx, f32 h, f32 hz, const char* color) {
  const f32 x0 = -hx, x1 = hx, y0 = 0.0f, y1 = h, z0 = -hz, z1 = hz;
  struct Face {
    f32 n[3];
    f32 p[4][3];
  };
  const Face faces[6] = {
      {{1, 0, 0}, {{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}},
      {{-1, 0, 0}, {{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}},
      {{0, 1, 0}, {{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}}},
      {{0, -1, 0}, {{x0, y0, z1}, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}}},
      {{0, 0, 1}, {{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}}},
      {{0, 0, -1}, {{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}},
  };
  std::vector<u8> bin;
  for (const Face& f : faces)
    for (const auto& p : f.p)
      for (const f32 v : p)
        put(bin, v);
  for (const Face& f : faces)
    for (u32 c = 0; c < 4; ++c)
      for (const f32 v : f.n)
        put(bin, v);
  for (u16 f = 0; f < 6; ++f) {
    const u16 b = static_cast<u16>(f * 4);
    const u16 tri[6] = {b, static_cast<u16>(b + 2), static_cast<u16>(b + 1),
                        b, static_cast<u16>(b + 3), static_cast<u16>(b + 2)};
    for (const u16 i : tri)
      put(bin, i);
  }
  char text[4096];
  std::snprintf(text, sizeof(text), R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [%s, 1],
                 "metallicFactor": 0, "roughnessFactor": 1}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                              "material": 0}]}],
  "buffers": [{"byteLength": %u, "uri": "data:application/octet-stream;base64,%s"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 288},
                  {"buffer": 0, "byteOffset": 288, "byteLength": 288},
                  {"buffer": 0, "byteOffset": 576, "byteLength": 72}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
     "min": [%g, 0, %g], "max": [%g, %g, %g]},
    {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"}]
})",
                color, static_cast<u32>(bin.size()), base64(bin).c_str(), static_cast<f64>(x0),
                static_cast<f64>(z0), static_cast<f64>(x1), static_cast<f64>(y1),
                static_cast<f64>(z1));
  return text;
}

// The pool: a basin of the renderer's ground (a bowl 6 m deep, 30 m in radius, in a plain), a body
// that fills it from its middle at 2 m below the plain, its water clear and calm. `objects` are
// the scene's instances after the meshes `log` (a yellow log 1.2 m by 0.3 m by 0.3 m) and `plate`
// (a white plate 6 m square, 0.2 m thick).
std::string pool_scene(const char* instances, bool flat, const char* water_extra = "") {
  char text[4096];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "refract-test",
  "meshes": [{"name": "log", "path": "log.gltf"}, {"name": "plate", "path": "plate.gltf"}],
  "instances": [%s],
  "terrain": {"size": 129, "extent": 128, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 30, "depth": 6}]},
  "water": [{"name": "pool", "level_m": -2,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 60}},
             "wind_follow": false, "wave_amplitude_m": %s, "wave_length_m": 2.0%s}]
})",
                instances, flat ? "0" : "0.01", water_extra);
  return text;
}

bool gpu_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool ok = probe.features().buffer_int64_atomics && probe.features().ray_query &&
                  probe.features().acceleration_structure;
  probe.destroy();
  if (!ok) MESSAGE("no 64-bit atomics or ray queries here: nothing under the water is traced");
  return ok;
}

struct Shot {
  CapturedFrame frame;
  u32 water_instance = ~0u;
  Stats stats;
};

struct Restore {
  ~Restore() {
    (void)tunables::apply_overrides("renderer.water.refract=1");
    (void)tunables::apply_overrides("renderer.water.refract_volume=true");
  }
};

struct ShotDesc {
  const char* scene = nullptr;
  WorldPos eye{};
  WorldPos target{};
  bool refract = true;
  bool volume = true;
  bool light_volumes = true;
  bool linear = false;  // capture the linear radiance (`DisplayEncoding::Linear`)
  const char* log_color = "0.85, 0.8, 0.2";
  u32 width = 480;
  u32 height = 320;
};

// Draws `desc.scene` `shots` times, each after a frame that fills the one-frame-late counts.
void shoot(const gfx::Device& device, const test::TempDir& tmp, const ShotDesc& d, u32 shots,
           Shot* out) {
  REQUIRE(tunables::apply_overrides(d.refract ? "renderer.water.refract=1"
                                              : "renderer.water.refract=0"));
  REQUIRE(tunables::apply_overrides(d.volume ? "renderer.water.refract_volume=true"
                                             : "renderer.water.refract_volume=false"));
  {
    std::ofstream file(tmp.file("log.gltf"), std::ios::binary);
    file << box_gltf(0.6f, 0.3f, 0.15f, d.log_color);
  }
  {
    std::ofstream file(tmp.file("plate.gltf"), std::ios::binary);
    file << box_gltf(3.0f, 0.2f, 3.0f, "0.8, 0.8, 0.8");
  }
  const std::string path = tmp.file("pool.json");
  {
    std::ofstream file(path, std::ios::binary);
    file << d.scene;
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  settings.off_frustum_casters = d.light_volumes;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  REQUIRE(resolved.casters);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  REQUIRE(scene.has_water());
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = d.width;
  rd.height = d.height;
  if (d.linear) rd.display = gfx::DisplayEncoding::Linear;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera.position = d.eye;
  frame.camera.target = d.target;
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.sky_time_s = 0.0;
  frame.world_time_s = 3.0;
  CaptureChannels channels;
  channels.ids = true;
  channels.light = d.linear;
  u32 water_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
  }
  CapturedFrame warm;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, warm, &error), error);
  for (u32 s = 0; s < shots; ++s) {
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].water_instance = water_instance;
    out[s].stats = renderer.stats();
  }
}

bool is_water(const CapturedFrame& f, u32 p, u32 water_instance) {
  return f.ids[p * k_id_words] == water_instance;
}

// Water pixels whose colour moved from `a` to `b` towards the log's yellow: red and green each up
// by at least three codes more than blue.
u32 yellower(const CapturedFrame& a, const CapturedFrame& b, u32 water_instance) {
  u32 n = 0;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    if (!is_water(a, p, water_instance)) continue;
    const i32 dr = i32{b.color[p * 4]} - i32{a.color[p * 4]};
    const i32 dg = i32{b.color[p * 4 + 1]} - i32{a.color[p * 4 + 1]};
    const i32 db = i32{b.color[p * 4 + 2]} - i32{a.color[p * 4 + 2]};
    if (dr - db >= 3 && dg - db >= 3) ++n;
  }
  return n;
}

struct Changes {
  u32 changed = 0;
  u32 off_water = 0;
};

Changes changes(const CapturedFrame& a, const CapturedFrame& b, u32 water_instance) {
  Changes c;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    if (std::memcmp(&a.color[p * 4], &b.color[p * 4], 3) == 0) continue;
    ++c.changed;
    if (!is_water(a, p, water_instance)) ++c.off_water;
  }
  return c;
}

}  // namespace

TEST_CASE("water refract: a pool over bare ground draws what it drew") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_refract_bare");
  Restore restore;
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  const std::string scene = pool_scene("", false);
  ShotDesc d;
  d.scene = scene.c_str();
  d.eye = WorldPos{-6.0, -0.5, -6.0};
  d.target = WorldPos{6.0, -5.0, 6.0};
  Shot off[1], on[1];
  d.refract = false;
  shoot(device, tmp, d, 1, off);
  d.refract = true;
  shoot(device, tmp, d, 1, on);
  device.destroy();
  REQUIRE(off[0].water_instance != ~0u);
  u32 water = 0;
  for (u32 p = 0; p < off[0].frame.width * off[0].frame.height; ++p)
    water += is_water(off[0].frame, p, off[0].water_instance) ? 1u : 0u;
  const Changes c = changes(off[0].frame, on[0].frame, off[0].water_instance);
  MESSAGE("bare pool: " << water << " water pixels, " << c.changed
                        << " changed by the refraction ray; casters " << off[0].stats.shadow_casters
                        << " -> " << on[0].stats.shadow_casters);
  CHECK(!off[0].stats.water_refract);
  CHECK(on[0].stats.water_refract);
  CHECK(water > 20000u);
  CHECK(off[0].frame.ids == on[0].frame.ids);
  CHECK(off[0].frame.color == on[0].frame.color);  // the ground stays the depth map's, to the bit
}

TEST_CASE("water refract: a log on the bed is seen through the surface") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_refract_log");
  Restore restore;
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // The log on the bed in the pool's middle, 4 m down; the eye 1.5 m over the water at the
  // south-west, looking down at it.
  const std::string scene =
      pool_scene(R"({"mesh": 0, "translation": [0, 0, 0], "ground": true})", false,
                 R"(, "absorption": [0.12, 0.05, 0.04], "scattering": [0.01, 0.012, 0.015])");
  ShotDesc d;
  d.scene = scene.c_str();
  d.eye = WorldPos{-5.0, -0.5, -5.0};
  d.target = WorldPos{0.0, -5.8, 0.0};
  Shot off[1], on[2];
  d.refract = false;
  shoot(device, tmp, d, 1, off);
  d.refract = true;
  shoot(device, tmp, d, 2, on);
  device.destroy();
  const Changes c = changes(off[0].frame, on[0].frame, off[0].water_instance);
  const u32 yellow = yellower(off[0].frame, on[0].frame, off[0].water_instance);
  MESSAGE("the log on the bed: the refraction ray changes " << c.changed << " pixels, " << yellow
                                                            << " of them towards its yellow");
  CHECK(off[0].frame.ids == on[0].frame.ids);     // shading: no surface moves
  CHECK(on[0].frame.color == on[1].frame.color);  // the same frame twice, the same bytes
  CHECK(c.off_water == 0u);                       // only the water's pixels change
  CHECK(yellow > 100u);
}

TEST_CASE("water refract: the submerged volume puts what is under the frame in the water") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_refract_volume");
  Restore restore;
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // The eye 2 m over the water, 7 m from the log in the pool's middle, looking level at a 60-degree
  // field of view: the frame's bottom edge meets the log's depth 9.7 m out and the log, 3.6 m under
  // the surface 7 m out, is wholly below it (its sphere 1.3 m from the bottom plane), while the
  // view refracted at the bottom rows lands on the bed 6.5 to 7.6 m out.
  const std::string scene =
      pool_scene(R"({"mesh": 0, "translation": [0, 0, 0], "ground": true})", true,
                 R"(, "absorption": [0.12, 0.05, 0.04], "scattering": [0.01, 0.012, 0.015])");
  ShotDesc d;
  d.scene = scene.c_str();
  d.eye = WorldPos{0.0, 0.0, -7.0};
  d.target = WorldPos{0.0, 0.0, 10.0};
  d.light_volumes = false;  // the sun's sweep would build the log as a caster anyway
  Shot cut[1], submerged[1];
  d.volume = false;
  shoot(device, tmp, d, 1, cut);
  d.volume = true;
  shoot(device, tmp, d, 1, submerged);
  device.destroy();
  const Changes c = changes(cut[0].frame, submerged[0].frame, cut[0].water_instance);
  const u32 yellow = yellower(cut[0].frame, submerged[0].frame, cut[0].water_instance);
  MESSAGE("the log under the frame: the volume changes "
          << c.changed << " pixels, " << yellow << " towards its yellow; casters "
          << cut[0].stats.shadow_casters << " -> " << submerged[0].stats.shadow_casters
          << ", drawn " << cut[0].stats.visible_pairs() << " -> "
          << submerged[0].stats.visible_pairs());
  CHECK(cut[0].stats.water_refract_views == 0u);
  CHECK(submerged[0].stats.water_refract_views == 1u);
  CHECK(submerged[0].stats.water_refract_floor_m < -2.0f);
  CHECK(cut[0].frame.ids == submerged[0].frame.ids);
  CHECK(c.off_water == 0u);
  CHECK(yellow > 20u);
  CHECK(submerged[0].stats.shadow_casters > cut[0].stats.shadow_casters);
  CHECK(submerged[0].stats.visible_pairs() == cut[0].stats.visible_pairs());
}

TEST_CASE("water refract: the sun under the water is dimmed as the reference says") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_refract_sun");
  Restore restore;
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // The log's top, 3 m under a flat, clear, unscattering surface, seen straight
  // down in linear light, with and without a plate high in the sun's direction shadowing where its
  // refracted beam leaves the surface. The difference is the sun's share alone, and its channels'
  // ratios are e^(-(sigma_c - sigma_g) (t + d / cos l)): the BSDF, the Fresnel terms, the sun's
  // strength and the exposure all cancel (the log's colour is in both captures and divided out by
  // the same ratio of the sky's light, which the plate does not touch).
  const f64 level = -2.0;
  const f64 top = level - 3.0;
  // The stand-in's sun (lighting.h), which a scene with no sky has at its own hour.
  const Vec3 sun = sun_direction(k_default_sun_azimuth_deg, k_default_sun_elevation_deg);
  const water_ref::Dvec3 sun_dir{static_cast<f64>(sun.x), static_cast<f64>(sun.y),
                                 static_cast<f64>(sun.z)};
  f64 cos_l = 1.0;
  const water_ref::Dvec3 l = water_ref::sun_under(sun_dir, cos_l);
  // Where the beam to the log's top leaves the surface, and the plate 8 m up the sun from there.
  const f64 run = 3.0 / cos_l;
  const f64 ex = l.x * run, ez = l.z * run;
  const f64 px = ex + sun_dir.x * (8.0 / sun_dir.y), pz = ez + sun_dir.z * (8.0 / sun_dir.y);
  char log_only[256];
  std::snprintf(log_only, sizeof(log_only), R"({"mesh": 0, "translation": [0, %.3f, 0]})",
                top - 0.3);
  char with_plate[512];
  std::snprintf(
      with_plate, sizeof(with_plate),
      R"({"mesh": 0, "translation": [0, %.3f, 0]}, {"mesh": 1, "translation": [%.3f, %.3f, %.3f]})",
      top - 0.3, px, level + 8.0, pz);
  const char* optics = R"(, "absorption": [0.3, 0.1, 0.05], "scattering": [0, 0, 0])";
  const std::string bright = pool_scene(log_only, true, optics);
  const std::string shaded = pool_scene(with_plate, true, optics);
  ShotDesc d;
  d.linear = true;
  d.log_color = "0.6, 0.6, 0.6";  // grey: the BSDF's two lobes the same in every channel
  d.eye = WorldPos{0.0, level + 3.0, 0.0};
  d.target = WorldPos{0.0, level - 10.0, 0.001};
  Shot lit[1], dark[1];
  d.scene = bright.c_str();
  shoot(device, tmp, d, 1, lit);
  d.scene = shaded.c_str();
  shoot(device, tmp, d, 1, dark);
  device.destroy();
  REQUIRE(!lit[0].frame.light.empty());
  // The middle 5 x 5 pixels: the log's top, seen through the water.
  const u32 w = lit[0].frame.width, h = lit[0].frame.height;
  f64 diff[3] = {0.0, 0.0, 0.0};
  for (u32 y = h / 2 - 2; y <= h / 2 + 2; ++y) {
    for (u32 x = w / 2 - 2; x <= w / 2 + 2; ++x) {
      const u32 p = y * w + x;
      REQUIRE(is_water(lit[0].frame, p, lit[0].water_instance));
      for (u32 c = 0; c < 3; ++c)
        diff[c] += static_cast<f64>(lit[0].frame.light[p * 3 + c]) -
                   static_cast<f64>(dark[0].frame.light[p * 3 + c]);
    }
  }
  REQUIRE(diff[1] > 0.0);
  // The eye's path to the top is the depth, straight down; the sun's beam has come d / cos l.
  const water_ref::Dvec3 extinction{0.3, 0.1, 0.05};
  const water_ref::Dvec3 beam =
      water_ref::submerged_sun(extinction, sun_dir, water_ref::Dvec3{1.0, 1.0, 1.0}, 3.0, cos_l);
  const water_ref::Dvec3 seen{beam.x * std::exp(-0.3 * 3.0), beam.y * std::exp(-0.1 * 3.0),
                              beam.z * std::exp(-0.05 * 3.0)};
  const f64 want_rg = seen.x / seen.y, want_bg = seen.z / seen.y;
  const f64 got_rg = diff[0] / diff[1];
  const f64 got_bg = diff[2] / diff[1];
  MESSAGE("the sun's share at the log's top, 3 m down: red/green "
          << got_rg << " (reference " << want_rg << "), blue/green " << got_bg << " (reference "
          << want_bg << ")");
  CHECK(got_rg == doctest::Approx(want_rg).epsilon(0.01));
  CHECK(got_bg == doctest::Approx(want_bg).epsilon(0.01));
}

#endif
