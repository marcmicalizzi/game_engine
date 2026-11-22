// A channel's water lit as the scene lets it, on a GPU (roadmap W161; docs/subsystems/water.md,
// "Translucent by volume"): under a roof, with the world-space cache reading, the water's ambient
// — its bed's, its volume's and its white's — falls by the share the cache gives the ground beside
// it (`scene_ambient` at the surface point), and with the cache off the water is the bytes it was.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
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
#include <string>
#include <string_view>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

bool traces_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool ok = probe.features().buffer_int64_atomics && probe.features().ray_query &&
                  probe.features().acceleration_structure;
  probe.destroy();
  if (!ok) MESSAGE("no 64-bit atomics or ray queries here: no cache and no traced shadow");
  return ok;
}

struct RestoreGi {
  ~RestoreGi() { (void)tunables::apply_overrides("renderer.gi.mode=off"); }
};

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

void put_f32(std::vector<u8>& out, f32 value) {
  u8 raw[sizeof(f32)];
  std::memcpy(raw, &value, sizeof(f32));
  out.insert(out.end(), raw, raw + sizeof(f32));
}

void put_u16(std::vector<u8>& out, u16 value) {
  u8 raw[sizeof(u16)];
  std::memcpy(raw, &value, sizeof(u16));
  out.insert(out.end(), raw, raw + sizeof(u16));
}

// A closed grey slab from (x0, y0, z0) to (x1, y1, z1), as a glTF with its buffer inline: 24
// corners (four a face, each with the face's normal) and 36 indices, counter-clockwise outside.
std::string slab_gltf(f32 x0, f32 x1, f32 y0, f32 y1, f32 z0, f32 z1) {
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
        put_f32(bin, v);
  for (const Face& f : faces)
    for (u32 c = 0; c < 4; ++c)
      for (const f32 v : f.n)
        put_f32(bin, v);
  for (u16 f = 0; f < 6; ++f) {
    const u16 b = static_cast<u16>(f * 4);
    const u16 tri[6] = {b, static_cast<u16>(b + 2), static_cast<u16>(b + 1),
                        b, static_cast<u16>(b + 3), static_cast<u16>(b + 2)};
    for (const u16 i : tri)
      put_u16(bin, i);
  }
  char text[8192];
  std::snprintf(text, sizeof(text), R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                 "metallicFactor": 0, "roughnessFactor": 0.9}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                              "material": 0}]}],
  "buffers": [{"byteLength": %u, "uri": "data:application/octet-stream;base64,%s"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 288},
                  {"buffer": 0, "byteOffset": 288, "byteLength": 288},
                  {"buffer": 0, "byteOffset": 576, "byteLength": 72}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
     "min": [%g, %g, %g], "max": [%g, %g, %g]},
    {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"}]
})",
                static_cast<u32>(bin.size()), base64(bin).c_str(), static_cast<f64>(x0),
                static_cast<f64>(y0), static_cast<f64>(z0), static_cast<f64>(x1),
                static_cast<f64>(y1), static_cast<f64>(z1));
  return text;
}

// The picture's linear light's luminance at pixel p.
f64 light_luminance(const CapturedFrame& f, u32 p) {
  return 0.2126 * static_cast<f64>(f.light[p * 3 + 0]) +
         0.7152 * static_cast<f64>(f.light[p * 3 + 1]) +
         0.0722 * static_cast<f64>(f.light[p * 3 + 2]);
}

// The instance a scene's channel and its terrain are drawn as.
void find_instances(const SceneData& data, u32& channel, u32& terrain) {
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh >= data.mesh_info.size()) continue;
    if (data.mesh_info[mesh].water.has_value() && data.mesh_info[mesh].water->channel) channel = i;
    if (std::string_view(data.mesh_info[mesh].origin) == "terrain") terrain = i;
  }
}

// S106's brook (channel_scene_tests.cpp, `k_brook`, where its closed forms are derived): a gentle
// reach at Froude 0.93 and a steep one at 1.73, so its water has white on it at the top tier.
constexpr const char* k_brook = R"([{"name": "brook",
  "points": [[-12, 2.0, 4], [0, 1.8, 4], [12, 1.0, 4]],
  "width_m": [3, 3, 2.74], "depth_m": [0.5, 0.4755, 0.35], "slope": [0.017, 0.03, 0.067],
  "discharge_m3s": [3.08, 3.08, 3.08], "speed_mps": [2.053, 2.159, 3.213],
  "froude": [0.927, 1.0, 1.734], "wet_left_m": [1.5, 1.5, 1.37],
  "wet_right_m": [1.5, 1.5, 1.37], "flags": [0, 1, 0], "seed": 3}])";

}  // namespace

// ---- What the scene lets through reaches under the water (W161, rule 2) ----------------------
// The brook over the renderer's dunes under a grey roof 40 m square and 5 m up, the eye under it
// looking down 45 degrees at the brook and the ground beside it, so every pixel it sees is in the
// roof's shade (the sun's traced shadow) and lit by what of the sky reaches under the roof. With
// the world-space cache reading (`renderer.gi.mode=sky`, its sky visibility, the stand-in's sky)
// the ground's ambient falls by the share the cells give it; the water's ambient — the bed's light
// coming down through the surface, the volume's scattered light, the white's — is the same
// hemisphere through the same cells at the surface point (`scene_ambient`), so it falls by the
// share the ground beside it takes. The two are compared as ratios of the same picture's light
// with the cache on and off, each over its own pixels, so the exposure cancels: |ln(water's ratio
// / ground's ratio)| is held under ln(1.35). What is left between the two is real: the surface
// stands 1 to 2 m above the dunes, nearer the roof, whose cells see a narrower band of sky; the
// reflection (Schlick, F under 0.06 at 45 degrees) is the traced roof's underside, lit through the
// same cache; and the white is lit on its own plane. Before W161 the water took the raw sky
// (`sky_ambient`), so its ratio was a little below 1 against the ground's share. Measured
// 2026-11-22 (RTX 5090, 320 x 240): before, the water 0.896 against the ground 0.300 (ln 1.09,
// 3.6 times the bound); after, the water 0.252 against the same 0.300 (ln 0.17). The cache-off
// colour's bytes hash the same at W161's rule 1 and rule 2 (e61fb2948277c59d on that card).
TEST_CASE("water channel: under a roof, the water's ambient is what the scene lets through") {
  if (!traces_here()) return;
  const test::TempDir tmp("water_channel_roof");
  RestoreGi restore;
  {
    std::ofstream roof(tmp.file("roof.gltf"), std::ios::binary);
    roof << slab_gltf(-20.0f, 20.0f, 5.0f, 5.4f, -20.0f, 20.0f);
  }
  const std::string path = tmp.file("scene.json");
  {
    std::ofstream out(path, std::ios::binary);
    out << R"({
  "format": "engine.scene.v1",
  "name": "channel-roof",
  "meshes": [{"name": "roof", "path": "roof.gltf"}],
  "instances": [{"mesh": 0, "translation": [0, 0, 4]}],
  "terrain": {"size": 65, "extent": 32, "seed": 4, "dune_height": 0.6, "dune_wavelength": 12},
  "water_channels": )"
        << k_brook << "\n}\n";
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  u32 channel = ~0u;
  u32 terrain = ~0u;
  find_instances(data, channel, terrain);
  REQUIRE(channel != ~0u);
  REQUIRE(terrain != ~0u);
  gfx::Device device;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  FrameDesc frame;
  frame.camera.position = WorldPos{-3.0, 4.2, 0.0};
  frame.camera.target = WorldPos{1.0, 0.8, 4.0};
  frame.world_time_s = 10.0;
  CaptureChannels channels;
  channels.ids = true;
  channels.light = true;
  CapturedFrame shots[2];
  const char* modes[2] = {"renderer.gi.mode=off", "renderer.gi.mode=sky"};
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
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = 320;
    rd.height = 240;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
    // The cache's pre-roll and a second of frames for its cells to settle; the same with it off.
    for (u32 i = 0; i < 48; ++i)
      REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
    REQUIRE_MESSAGE(renderer.capture(frame, channels, shots[s], &error), error);
    renderer.destroy();
    scene.destroy();
  }
  device.destroy();
  REQUIRE(shots[0].ids == shots[1].ids);
  REQUIRE(shots[0].light.size() == shots[1].light.size());
  f64 water[2] = {};
  f64 ground[2] = {};
  u32 water_pixels = 0;
  u32 ground_pixels = 0;
  for (u32 p = 0; p < shots[0].width * shots[0].height; ++p) {
    const u32 id = shots[0].ids[p * k_id_words];
    if (id != channel && id != terrain) continue;
    for (u32 s = 0; s < 2; ++s)
      (id == channel ? water : ground)[s] += light_luminance(shots[s], p);
    ++(id == channel ? water_pixels : ground_pixels);
  }
  // The cache-off picture's bytes, for the record: the same before and after W161's rule 2.
  const u64 off_bytes = hash_bytes(shots[0].color.data(), shots[0].color.size());
  REQUIRE(water_pixels > 2000u);
  REQUIRE(ground_pixels > 2000u);
  REQUIRE(water[0] > 0.0);
  REQUIRE(ground[0] > 0.0);
  const f64 water_ratio = water[1] / water[0];
  const f64 ground_ratio = ground[1] / ground[0];
  char hex[32];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(off_bytes));
  MESSAGE("under the roof, cache on over off: the water's light "
          << water_ratio << " over " << water_pixels << " pixels, the ground's " << ground_ratio
          << " over " << ground_pixels
          << "; the cache-off colour's "
             "bytes hash "
          << hex);
  // The cache darkens the shade: a roof that let the whole sky through would test nothing.
  CHECK(ground_ratio < 0.7);
  CHECK(std::abs(std::log(water_ratio / ground_ratio)) < std::log(1.35));
}
