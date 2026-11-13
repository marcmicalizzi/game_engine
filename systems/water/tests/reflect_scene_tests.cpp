// The water reflects the scene, on a GPU (docs/subsystems/water.md, "Shading"; roadmap R129): a
// pool that fills a wide basin of the renderer's ground, a tall red box standing on the far shore,
// drawn through the renderer's reader and the resolve's ray-traced water module with the
// reflection on and off (`renderer.water.reflect`). The box's red is in the water's pixels with it
// on, and only there; no id moves; the same frame twice is the same bytes. And with the camera low
// over the water looking down, the box wholly above the frame's top edge: its reflection is there
// only when the mirrored caster volume builds it into the frame's structures
// (`renderer.water.reflect_volume`) — the one picture that proves the volume earns its keep.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/ground_masks.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

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

// A closed box, 2 m by 8 m tall by 2 m, standing on its base at the origin, red, as a glTF with its
// buffer inline: 24 corners (four a face, each with the face's normal) and 36 indices.
std::string box_gltf() {
  const f32 x0 = -1.0f, x1 = 1.0f, y0 = 0.0f, y1 = 8.0f, z0 = -1.0f, z1 = 1.0f;
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
  // Two triangles a face, wound counter-clockwise seen from outside.
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
  "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.05, 0.03, 1],
                 "metallicFactor": 0, "roughnessFactor": 0.8}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                              "material": 0}]}],
  "buffers": [{"byteLength": %u, "uri": "data:application/octet-stream;base64,%s"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 288},
                  {"buffer": 0, "byteOffset": 288, "byteLength": 288},
                  {"buffer": 0, "byteOffset": 576, "byteLength": 72}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
     "min": [-1, 0, -1], "max": [1, 8, 1]},
    {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"}]
})",
                static_cast<u32>(bin.size()), base64(bin).c_str());
  return text;
}

// A wide basin of the renderer's ground (no dunes: a bowl 6 m deep, 30 m in radius, in a plain), a
// body that fills it from its middle at 2 m below the plain, and the box standing on the ground at
// (14, 14), past the waterline on the far side. The shore's wet band `wet_m` wide (the schema's
// 0.3 unless a case says otherwise; 0 none).
std::string pool_scene(f32 wet_m) {
  char text[1024];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "reflect-test",
  "meshes": [{"name": "box", "path": "box.gltf"}],
  "instances": [{"mesh": 0, "translation": [14, 0, 14], "ground": true}],
  "terrain": {"size": 129, "extent": 128, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 30, "depth": 6}]},
  "water": [{"name": "pool", "level_m": -2,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 60}},
             "wind_follow": false, "wave_amplitude_m": 0.01, "wave_length_m": 2.0,
             "wet_m": %g}]
})",
                static_cast<f64>(wet_m));
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
  if (!ok) MESSAGE("no 64-bit atomics or ray queries here: the water reflects the sky alone");
  return ok;
}

struct Shot {
  CapturedFrame frame;
  u32 water_instance = ~0u;
  u32 box_instance = ~0u;
  Stats stats;
};

// Puts the tunables back whatever a case does.
struct Restore {
  ~Restore() {
    (void)tunables::apply_overrides("renderer.water.reflect=1");
    (void)tunables::apply_overrides("renderer.water.reflect_volume=true");
    (void)tunables::apply_overrides("renderer.water.reflect_shadow=1");
  }
};

// Draws the pool from `eye` towards `target` with the reflection as `reflect` and the mirrored
// volume as `volume`, `shots` times (each after a frame that fills the one-frame-late counts).
void shoot(const gfx::Device& device, const test::TempDir& tmp, WorldPos eye, WorldPos target,
           bool reflect, bool volume, u32 shots, Shot* out, bool light_volumes = true,
           f32 wet_m = 0.3f) {
  REQUIRE(
      tunables::apply_overrides(reflect ? "renderer.water.reflect=1" : "renderer.water.reflect=0"));
  REQUIRE(tunables::apply_overrides(volume ? "renderer.water.reflect_volume=true"
                                           : "renderer.water.reflect_volume=false"));
  {
    std::ofstream file(tmp.file("box.gltf"), std::ios::binary);
    file << box_gltf();
  }
  const std::string path = tmp.file("pool.json");
  {
    std::ofstream file(path, std::ios::binary);
    file << pool_scene(wet_m);
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  // The sun's off-frustum caster volume would build an object outside the view into the structures
  // anyway when it stands in the sweep; the mirror case turns it off so the water's volume is
  // alone.
  settings.off_frustum_casters = light_volumes;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  REQUIRE(resolved.casters);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  REQUIRE(scene.has_water());
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera.position = eye;
  frame.camera.target = target;
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.sky_time_s = 0.0;
  frame.world_time_s = 3.0;
  CaptureChannels channels;
  channels.ids = true;
  u32 water_instance = ~0u;
  u32 box_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
    if (mesh == 0) box_instance = i;
  }
  CapturedFrame warm;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, warm, &error), error);
  for (u32 s = 0; s < shots; ++s) {
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].water_instance = water_instance;
    out[s].box_instance = box_instance;
    out[s].stats = renderer.stats();
  }
}

bool is_water(const CapturedFrame& f, u32 p, u32 water_instance) {
  return f.ids[p * k_id_words] == water_instance;
}

// Pixels of the water whose colour moved towards the box's red from `a` to `b`: red up by at least
// four codes more than either of the others.
u32 redder(const CapturedFrame& a, const CapturedFrame& b, u32 water_instance) {
  u32 n = 0;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    if (!is_water(a, p, water_instance)) continue;
    const i32 dr = i32{b.color[p * 4]} - i32{a.color[p * 4]};
    const i32 dg = i32{b.color[p * 4 + 1]} - i32{a.color[p * 4 + 1]};
    const i32 db = i32{b.color[p * 4 + 2]} - i32{a.color[p * 4 + 2]};
    if (dr - (dg > db ? dg : db) >= 4) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("water reflect: the far shore's box is in the water, and nothing else changes") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_reflect_far");
  Restore restore;
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // From the south-west shore, 1.5 m over the water, across the pool at the box.
  const WorldPos eye{-6.0, -0.5, -6.0};
  const WorldPos target{14.0, -3.3, 14.0};
  Shot sky[1], scene[2], full[1], dry_sky[1], dry_scene[1];
  shoot(device, tmp, eye, target, false, true, 1, sky);
  shoot(device, tmp, eye, target, true, true, 2, scene);
  // The hit's shadow ray at every pixel rather than half of them: what the half rate changes.
  REQUIRE(tunables::apply_overrides("renderer.water.reflect_shadow=2"));
  shoot(device, tmp, eye, target, true, true, 1, full);
  REQUIRE(tunables::apply_overrides("renderer.water.reflect_shadow=1"));
  // And the shore with no wet band: nothing but the water may change.
  shoot(device, tmp, eye, target, false, true, 1, dry_sky, true, 0.0f);
  shoot(device, tmp, eye, target, true, true, 1, dry_scene, true, 0.0f);
  device.destroy();
  const CapturedFrame& a = sky[0].frame;
  const CapturedFrame& b = scene[0].frame;
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(scene[0].water_instance != ~0u);
  CHECK(a.ids == b.ids);                   // the reflection is shading: no surface moves
  CHECK(b.color == scene[1].frame.color);  // and the same frame twice is the same bytes
  CHECK(!sky[0].stats.water_reflect);
  CHECK(scene[0].stats.water_reflect);
  CHECK(scene[0].stats.water_reflect_views == 1u);
  CHECK(scene[0].stats.water_reflect_level_m < 0.0f);
  const u32 pixels = a.width * a.height;
  // Until S99 nothing but the water reflected, and the reflection could change no other pixel. The
  // wet band's film reflects too, through the same traced reflection (water.md, "The shore"), so
  // with the band at width 0 the check is the exact one it was, and with the band on every pixel
  // off the water that the reflection changes must be one the band itself changes (with the
  // reflection off), and on the ground: a subset, never a count.
  const CapturedFrame& d0 = dry_sky[0].frame;
  const CapturedFrame& d1 = dry_scene[0].frame;
  REQUIRE(d0.color.size() == a.color.size());
  CHECK(d0.ids == a.ids);
  CHECK(d1.ids == a.ids);
  u32 water = 0, changed = 0, off_water = 0, off_band = 0, off_ground = 0, box = 0, dry_off = 0;
  for (u32 p = 0; p < pixels; ++p) {
    const bool w = is_water(a, p, scene[0].water_instance);
    water += w ? 1u : 0u;
    const u32 id = a.ids[p * k_id_words];
    box += id == scene[0].box_instance ? 1u : 0u;
    if (!w && std::memcmp(&d0.color[p * 4], &d1.color[p * 4], 3) != 0) ++dry_off;
    if (std::memcmp(&a.color[p * 4], &b.color[p * 4], 3) == 0) continue;
    ++changed;
    if (w) continue;
    ++off_water;
    if (std::memcmp(&a.color[p * 4], &d0.color[p * 4], 3) == 0) ++off_band;
    if (id == k_no_id || id == scene[0].box_instance) ++off_ground;
  }
  const u32 red = redder(a, b, scene[0].water_instance);
  MESSAGE("pool: " << water << " water pixels, the box " << box << "; the reflection changes "
                   << changed << ", " << red << " of them towards the box's red, " << off_water
                   << " off the water (the wet band's film); casters "
                   << sky[0].stats.shadow_casters << " -> " << scene[0].stats.shadow_casters);
  u32 half_rate = 0, half_off_water = 0;
  for (u32 p = 0; p < pixels; ++p) {
    if (std::memcmp(&full[0].frame.color[p * 4], &b.color[p * 4], 3) == 0) continue;
    ++half_rate;
    if (!is_water(a, p, scene[0].water_instance)) ++half_off_water;
  }
  MESSAGE("the half rate: " << half_rate << " pixels differ from the shadow ray at every pixel");
  CHECK(half_off_water == 0u);
  CHECK(water > 20000u);
  CHECK(box > 200u);
  CHECK(red > 100u);
  CHECK(dry_off == 0u);     // no band: the reflection changes the water alone
  CHECK(off_band == 0u);    // with one: off the water, only pixels the band changes
  CHECK(off_ground == 0u);  // and those on the ground
}

TEST_CASE("water reflect: the mirrored caster volume puts what is above the frame in the water") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_reflect_mirror");
  Restore restore;
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // Half a metre over the water in the pool's middle, looking down 40 degrees towards the box: at a
  // 60-degree field of view the frame's top edge is 10 degrees under the horizon, so the box (its
  // base about level with the eye, 20 m off) is wholly above the frame, and the upper half of its
  // mirror image, 10 to 24 degrees under the horizon, is in it.
  const WorldPos eye{0.0, -1.5, 0.0};
  const f64 down = 40.0 * 3.14159265358979 / 180.0;
  const WorldPos target{10.0 * std::cos(down) * 0.70710678, -1.5 - 10.0 * std::sin(down),
                        10.0 * std::cos(down) * 0.70710678};
  Shot sky[1], cut[1], mirrored[1];
  shoot(device, tmp, eye, target, false, true, 1, sky, false);
  shoot(device, tmp, eye, target, true, false, 1, cut, false);
  shoot(device, tmp, eye, target, true, true, 1, mirrored, false);
  device.destroy();
  const u32 pixels = sky[0].frame.width * sky[0].frame.height;
  u32 box = 0;
  for (u32 p = 0; p < pixels; ++p)
    box += sky[0].frame.ids[p * k_id_words] == sky[0].box_instance ? 1u : 0u;
  CHECK(box == 0u);  // the box is not in the picture
  CHECK(sky[0].frame.ids == mirrored[0].frame.ids);
  CHECK(cut[0].stats.water_reflect_views == 0u);
  CHECK(mirrored[0].stats.water_reflect_views == 1u);
  // The reflected ground is redder than the sky it replaces with or without the volume, so what the
  // volume adds is measured against the picture without it: the box's red, on water pixels alone.
  const u32 red_cut = redder(sky[0].frame, cut[0].frame, sky[0].water_instance);
  const u32 red_added = redder(cut[0].frame, mirrored[0].frame, sky[0].water_instance);
  u32 changed = 0, off_water = 0;
  for (u32 p = 0; p < pixels; ++p) {
    if (std::memcmp(&cut[0].frame.color[p * 4], &mirrored[0].frame.color[p * 4], 3) == 0) continue;
    ++changed;
    if (!is_water(sky[0].frame, p, sky[0].water_instance)) ++off_water;
  }
  MESSAGE("the box above the frame: the reflection without the mirrored volume reddens "
          << red_cut << " water pixels (the ground's), the volume " << red_added << " more of "
          << changed << " it changes; casters " << cut[0].stats.shadow_casters << " -> "
          << mirrored[0].stats.shadow_casters);
  CHECK(red_added > 100u);
  CHECK(off_water == 0u);
  CHECK(mirrored[0].stats.shadow_casters > cut[0].stats.shadow_casters);
}

#endif

// ---- A ray's hit on the ground takes the ground's material (R129's second step) ---------------
// Outside the dunes' guard: a ground of its own (scene_gen's registrar pattern) rather than the
// renderer's. A level pool fills a bowl whose banks are a two-layer palette under ground masks: the
// first layer, grass, south of z = 0 and the second, sand, north of it, so the far bank the camera
// looks across the water at is sand under the masks' whole weight and grass carries none of it
// there. Until R129's second step a terrain hit took the record's first layer's mean, and the far
// bank reflected green; it reflects the sand's colour now, against the directly viewed bank within
// a bound derived below.
namespace {

// The bowl: level at -3 m within 15 m of the origin, rising 0.4 m a metre beyond, so a pool at
// -1 m meets its banks 20 m out and the far bank stands 16.6 m high at the terrain's edge (64 m).
f32 bowl(f64 x, f64 z) noexcept {
  const f64 r = std::sqrt(x * x + z * z);
  return static_cast<f32>(-3.0 + 0.4 * std::max(r - 15.0, 0.0));
}
f32 bank_height(const void*, f32 x, f32 z) noexcept {
  return bowl(static_cast<f64>(x), static_cast<f64>(z));
}
f32 bank_height_mm(const void*, f64, i64 x, i64 z) noexcept {
  return bowl(static_cast<f64>(x) * 0.001, static_cast<f64>(z) * 0.001);
}
f32 bank_surface_mm(const void*, i64 x, i64 z) noexcept {
  return bowl(static_cast<f64>(x) * 0.001, static_cast<f64>(z) * 0.001);
}

// The mask level: 16 m tiles of 8 cells at 2 m over [-64, 64] m, palette layer 0 (grass) south of
// z = 0 and layer 1 (sand) north of it, half each on the seam's points.
constexpr u32 k_cells = 8;
constexpr i64 k_spacing_mm = 2000;
constexpr i32 k_tile_lo = -4;
constexpr i32 k_tile_hi = 3;
bool bank_mask_info(const void*, scene_gen::GroundMaskInfo& out) noexcept {
  out = scene_gen::GroundMaskInfo{};
  out.spacing_mm = k_spacing_mm;
  out.cells = k_cells;
  out.tile_min_x = out.tile_min_z = k_tile_lo;
  out.tile_max_x = out.tile_max_z = k_tile_hi;
  out.palette = 2;
  out.budget = 2;
  return true;
}
bool bank_mask_present(const void*, i32 tx, i32 tz) noexcept {
  return tx >= k_tile_lo && tx <= k_tile_hi && tz >= k_tile_lo && tz <= k_tile_hi;
}
bool bank_mask_tile(const void*, i32 tx, i32 tz, std::span<u32> words) noexcept {
  if (!bank_mask_present(nullptr, tx, tz)) return false;
  for (u32& w : words)
    w = 0;
  words[0] = 0u | (1u << 8) | 0xFFFF0000u;
  for (u32 m = 0; m < scene_gen::mask_mips(k_cells); ++m) {
    const u32 side = scene_gen::mask_mip_side(k_cells, m);
    for (u32 b = 0; b < side; ++b)
      for (u32 a = 0; a < side; ++a) {
        const i64 z = static_cast<i64>(tz) * k_cells + (static_cast<i64>(b) << m);
        const u32 grass = z < 0 ? 255u : (z == 0 ? 128u : 0u);
        const u32 p = scene_gen::mask_mip_offset(k_cells, m) + b * side + a;
        words[1 + p] = grass | ((255u - grass) << 8);
      }
  }
  return true;
}
constexpr scene_gen::GroundMaskOps k_bank_mask_ops{
    .present = &bank_mask_present, .info = &bank_mask_info, .tile = &bank_mask_tile};
bool bank_masks(const void*, scene_gen::GroundMasks& out, std::string*) {
  out = scene_gen::GroundMasks{&k_bank_mask_ops, nullptr};
  return true;
}
constexpr scene_gen::GroundOps k_bank_ops{.destroy = nullptr,
                                          .height = &bank_height,
                                          .floor = nullptr,
                                          .grid = nullptr,
                                          .evaluate = nullptr,
                                          .travel_m = nullptr,
                                          .wind = nullptr,
                                          .transport = nullptr,
                                          .make_rings = nullptr,
                                          .open_tiles = nullptr,
                                          .record = nullptr,
                                          .height_mm = &bank_height_mm,
                                          .surface_mm = &bank_surface_mm,
                                          .floor_mm = &bank_surface_mm,
                                          .tile_source = nullptr,
                                          .masks = &bank_masks};
bool bank_make(const scene::Terrain&, const scene_gen::Context&, scene_gen::GroundProvider& out,
               std::string*) {
  out = scene_gen::GroundProvider(&k_bank_ops, nullptr);
  return true;
}
constexpr scene_gen::GroundProviderDesc k_bank_provider{
    .name = "reflect_bank_probe", .make = &bank_make, .flags = 0};
const scene_gen::Registrar k_bank_registrar{k_bank_provider};

// The palette's two layers: their tints are their mean colours (no images).
constexpr f32 k_grass[3] = {0.10f, 0.35f, 0.06f};
constexpr f32 k_sand[3] = {0.65f, 0.52f, 0.32f};

// A pool at -1 m filling the bowl from its middle: black water (no bottom to see), a mirror (no
// waves), no wet band, drawn at the low tier (whose body is the shallow and deep colours, where the
// volume tier would add its own scattered blue-green), so a water pixel is its Fresnel times what
// its reflection met.
const char* k_scene = R"({
  "format": "engine.scene.v1",
  "name": "reflect-ground-test",
  "terrain": {"provider": "reflect_bank_probe", "size": 65, "extent": 128},
  "water": [{"name": "pool", "level_m": -1,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 60}},
             "shallow": [0, 0, 0], "deep": [0, 0, 0], "wind_follow": false,
             "wave_amplitude_m": 0, "wet_m": 0, "foam_m": 0}]
})";

bool ray_queries_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool ok = probe.features().buffer_int64_atomics && probe.features().ray_query &&
                  probe.features().acceleration_structure;
  probe.destroy();
  if (!ok) MESSAGE("no 64-bit atomics or ray queries here: the water reflects the sky alone");
  return ok;
}

struct RestoreReflect {
  ~RestoreReflect() { (void)tunables::apply_overrides("renderer.water.reflect=1"); }
};

f64 linear(u8 code) {
  const f64 c = static_cast<f64>(code) / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

}  // namespace

TEST_CASE(
    "water reflect: a far bank of sand under the masks reflects as sand, not the first layer") {
  if (!ray_queries_here()) return;
  const test::TempDir tmp("water_reflect_ground");
  RestoreReflect restore;
  std::string error;
  const std::string path = tmp.file("bank.json");
  {
    std::ofstream file(path, std::ios::binary);
    file << k_scene;
  }
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  desc.cache = false;
  desc.terrain.materials.emplace();
  desc.terrain.materials->layer_budget = 2;
  for (const auto& [name, tint] : {std::pair{"grass", k_grass}, std::pair{"sand", k_sand}}) {
    scene::TerrainMaterialLayer l;
    l.name = name;
    l.tint = Vec3{tint[0], tint[1], tint[2]};
    desc.terrain.materials->layers.push_back(l);
  }
  desc.terrain.masks.emplace();
  // The scene is loaded before the device is opened: the device holds the machine's GPU lock, and
  // the load (the ground, the fill, the clusters) is the CPU's.
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  gfx::Device device;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  settings.water_quality = WaterQuality::Low;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  REQUIRE(scene.has_water());
  REQUIRE(scene.terrain_mask_tiles() > 0);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  u32 water_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
  }
  REQUIRE(water_instance != ~0u);
  // On the south bank, 1.7 m over the ground, across the pool at the far bank; the sun (the
  // stand-in's, north-east and 53 degrees up) lights the bank and its glint off the mirror is under
  // the frame.
  FrameDesc frame;
  frame.camera.position = WorldPos{0.0, static_cast<f64>(bowl(0.0, -24.0)) + 1.7, -24.0};
  frame.camera.target = WorldPos{0.0, 1.0, 20.0};
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.world_time_s = 0.0;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shots[2];
  for (u32 s = 0; s < 2; ++s) {
    REQUIRE(tunables::apply_overrides(s == 0 ? "renderer.water.reflect=0"
                                             : "renderer.water.reflect=1"));
    CapturedFrame warm;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, warm, &error), error);
    REQUIRE_MESSAGE(renderer.capture(frame, channels, shots[s], &error), error);
  }
  renderer.destroy();
  scene.destroy();
  device.destroy();
  const CapturedFrame& sky = shots[0];
  const CapturedFrame& refl = shots[1];
  REQUIRE(sky.ids == refl.ids);
  // The middle half of the columns, clear of the bowl's sides; the far bank is the ground above the
  // water's top row there, the reflected bank the water pixels the reflection changes.
  const u32 w = refl.width;
  const u32 h = refl.height;
  u32 water_top = h;
  for (u32 y = 0; y < h && water_top == h; ++y)
    for (u32 x = w / 4; x < 3 * w / 4; ++x)
      if (refl.ids[(y * w + x) * k_id_words] == water_instance) {
        water_top = y;
        break;
      }
  REQUIRE(water_top < h);
  f64 bank[3] = {}, mirrored[3] = {};
  u32 banks = 0, mirrors = 0;
  for (u32 y = 0; y < h; ++y)
    for (u32 x = w / 4; x < 3 * w / 4; ++x) {
      const u32 p = y * w + x;
      const u32 id = refl.ids[p * k_id_words];
      const u8* c = &refl.color[p * 4];
      if (id == water_instance) {
        const u8* o = &sky.color[p * 4];
        // A pixel whose reflection met the bank: changed by the ray, and in the bank's codes rather
        // than the stand-in sky's (at least 24 codes in its strongest channel).
        bool moved = false;
        for (u32 k = 0; k < 3; ++k)
          moved = moved || std::abs(i32{c[k]} - i32{o[k]}) >= 3;
        if (!moved || std::max({c[0], c[1], c[2]}) < 24) continue;
        for (u32 k = 0; k < 3; ++k)
          mirrored[k] += linear(c[k]);
        ++mirrors;
      } else if (id != k_no_id && y + 4 < water_top && y > 2) {
        for (u32 k = 0; k < 3; ++k)
          bank[k] += linear(c[k]);
        ++banks;
      }
    }
  MESSAGE("the far bank: " << banks << " pixels seen, " << mirrors << " in the water");
  REQUIRE(banks > 2000u);
  REQUIRE(mirrors > 2000u);
  // Chromaticity: a water pixel is the Fresnel (a scalar) times the bank's radiance, so its
  // colour's ratios are the bank's. The bank seen and the bank reflected are the same albedo under
  // the same sun and sky, so their red-to-green and blue-to-green ratios agree up to (the bound):
  // the 8-bit store's half code over the mean green code of either (under 1% each at the codes
  // here), the dither's code, and the hit's shading against the resolve's — its specular lobe seen
  // from below rather than from the eye, a white share of at most F0 = 0.04 over albedos of 0.3 to
  // 0.65, which pulls a ratio towards 1 by at most 0.04 / 0.32 = 12.5%. So |ln(ratio reflected /
  // ratio seen)| is held under ln(1.15). Grass in the water would put the red-to-green ratio at
  // grass's 0.29 over sand's 1.25 of the bank's: ln 4.4, thirty times the bound.
  const f64 seen_rg = bank[0] / bank[1], seen_bg = bank[2] / bank[1];
  const f64 refl_rg = mirrored[0] / mirrored[1], refl_bg = mirrored[2] / mirrored[1];
  const f64 bound = std::log(1.15);
  const f64 grass_rg = seen_rg * (static_cast<f64>(k_grass[0]) / static_cast<f64>(k_grass[1])) /
                       (static_cast<f64>(k_sand[0]) / static_cast<f64>(k_sand[1]));
  MESSAGE("red/green: seen " << seen_rg << ", reflected " << refl_rg << " (grass would be "
                             << grass_rg << "); blue/green: seen " << seen_bg << ", reflected "
                             << refl_bg << "; bound ln(1.15)");
  CHECK(std::abs(std::log(refl_rg / seen_rg)) < bound);
  CHECK(std::abs(std::log(refl_bg / seen_bg)) < bound);
  CHECK(refl_rg > 1.0);  // red over green: sand, where the first layer's grass is green over red
}

// ---- A bounce off the ground takes the ground's material too (R69 stage 5) ---------------------
// The same bank, with a grey box standing on its sand: the world-space cache's bounce
// (`renderer.gi.mode=bounce`) lights the box's sides with the sun off the ground round it. Until
// stage 5 the bounce's hit on a terrain tile read the root material's base colour, the record's
// first layer, so the sand bounced the grass's green onto the box; the cache's terrain module
// (gi_update_terrain.slang) reads the masks' own layers there (`terrain_hit_material`, R129's).
// What the bounce adds is the picture under `bounce` less the picture under `sky`, whose rays
// bring the sky through the same visibility and nothing from the ground: over the box's pixels
// its red-to-green and blue-to-green ratios are the sand's (1.25 and 0.62) where the grass's are
// 0.29 and 0.17. The two pictures are differenced after the display's curve, which pulls a ratio
// towards 1 (2026-11-10, RTX 5090: 0.93 and 0.60), so the check is not the sand's ratio but which
// of the two layers each ratio is nearer in the logarithm: past the geometric mean of the two.
namespace {

struct RestoreGi {
  ~RestoreGi() { (void)tunables::apply_overrides("renderer.gi.mode=off"); }
};

}  // namespace

TEST_CASE(
    "gi cache: a bounce off a bank of sand under the masks is sand's, not the first layer's") {
  if (!ray_queries_here()) return;
  const test::TempDir tmp("gi_bounce_ground");
  RestoreGi restore;
  std::string error;
  {
    // The far shore's box, grey: what it shows is the light that reaches it.
    std::string box = box_gltf();
    const std::string red = "[0.9, 0.05, 0.03, 1]";
    const usize at = box.find(red);
    REQUIRE(at != std::string::npos);
    box.replace(at, red.size(), "[0.8, 0.8, 0.8, 1]");
    std::ofstream file(tmp.file("box.gltf"), std::ios::binary);
    file << box;
  }
  const std::string path = tmp.file("bank.json");
  {
    std::ofstream file(path, std::ios::binary);
    file << R"({
  "format": "engine.scene.v1",
  "name": "gi-ground-test",
  "meshes": [{"name": "box", "path": "box.gltf"}],
  "instances": [{"mesh": 0, "translation": [0, 0, 30], "ground": true}],
  "terrain": {"provider": "reflect_bank_probe", "size": 65, "extent": 128}
})";
  }
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  desc.cache = false;
  desc.terrain.materials.emplace();
  desc.terrain.materials->layer_budget = 2;
  for (const auto& [name, tint] : {std::pair{"grass", k_grass}, std::pair{"sand", k_sand}}) {
    scene::TerrainMaterialLayer l;
    l.name = name;
    l.tint = Vec3{tint[0], tint[1], tint[2]};
    desc.terrain.materials->layers.push_back(l);
  }
  desc.terrain.masks.emplace();
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  u32 box_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i)
    if (data.instances[i].mesh == 0) box_instance = i;
  REQUIRE(box_instance != ~0u);
  gfx::Device device;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // From the pool's side of the box, a little west: its south and west faces, and the sand the
  // sun lights in front of them.
  FrameDesc frame;
  frame.camera.position = WorldPos{-9.0, static_cast<f64>(bowl(0.0, 30.0)) + 5.0, 12.0};
  frame.camera.target = WorldPos{0.0, static_cast<f64>(bowl(0.0, 30.0)) + 3.0, 30.0};
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shots[2];
  const char* modes[2] = {"renderer.gi.mode=sky", "renderer.gi.mode=bounce"};
  for (u32 s = 0; s < 2; ++s) {
    REQUIRE(tunables::apply_overrides(modes[s]));
    RenderSettings settings;
    settings.shadows = ShadowMode::RayTraced;
    settings.lights = false;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
    REQUIRE(scene.has_terrain_materials());
    REQUIRE(scene.terrain_mask_tiles() > 0);
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = 320;
    rd.height = 240;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
    // The pre-roll's samples on the first frame, and a second of frames more for the bounce's
    // own bounces.
    for (u32 i = 0; i < 48; ++i)
      REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
    REQUIRE_MESSAGE(renderer.capture(frame, channels, shots[s], &error), error);
    renderer.destroy();
    scene.destroy();
  }
  device.destroy();
  REQUIRE(shots[0].ids == shots[1].ids);
  f64 added[3] = {};
  u32 pixels = 0;
  for (u32 p = 0; p < shots[1].width * shots[1].height; ++p) {
    if (shots[1].ids[p * k_id_words] != box_instance) continue;
    for (u32 k = 0; k < 3; ++k)
      added[k] += linear(shots[1].color[p * 4 + k]) - linear(shots[0].color[p * 4 + k]);
    ++pixels;
  }
  REQUIRE(pixels > 1500u);
  REQUIRE(added[1] > 0.0);
  const f64 sand_rg = static_cast<f64>(k_sand[0]) / static_cast<f64>(k_sand[1]);
  const f64 grass_rg = static_cast<f64>(k_grass[0]) / static_cast<f64>(k_grass[1]);
  MESSAGE("what the bounce adds over "
          << pixels << " pixels of the box: red/green " << added[0] / added[1] << ", blue/green "
          << added[2] / added[1] << " (sand " << sand_rg << ", grass " << grass_rg << ")");
  const f64 sand_bg = static_cast<f64>(k_sand[2]) / static_cast<f64>(k_sand[1]);
  const f64 grass_bg = static_cast<f64>(k_grass[2]) / static_cast<f64>(k_grass[1]);
  CHECK(added[0] / added[1] > std::sqrt(sand_rg * grass_rg));
  CHECK(added[2] / added[1] > std::sqrt(sand_bg * grass_bg));
}
