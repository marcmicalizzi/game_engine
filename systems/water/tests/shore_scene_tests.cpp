// The shore on a GPU (docs/subsystems/water.md, "The shore"; roadmap S97): a body that fills a
// basin of the renderer's ground, drawn through the renderer's reader and the resolve's water
// modules. The wet band darkens terrain pixels and nothing else, more of them the wider it is and
// none with it off; the foam line changes the water's pixels and nothing else, and none with it
// off; the same frame twice is the same bytes.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_WATER_TESTS_DUNES

namespace {

// A basin of the renderer's ground (no dunes: a smooth bowl 4 m deep in a plain), and a body that
// fills it from its middle at 1.5 m below the plain, with the shore's two fields given.
std::string basin_scene(f32 wet_m, f32 foam_m) {
  char text[1024];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "shore-test",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]},
  "water": [{"name": "basin", "level_m": -1.5,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 30}},
             "wind_follow": false, "wave_amplitude_m": 0.03, "wave_length_m": 1.5,
             "wet_m": %g, "foam_m": %g}]
})",
                static_cast<f64>(wet_m), static_cast<f64>(foam_m));
  return text;
}

bool gpu_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool atomics = probe.features().buffer_int64_atomics;
  probe.destroy();
  if (!atomics) MESSAGE("no 64-bit buffer atomics here");
  return atomics;
}

struct Shot {
  CapturedFrame frame;
  u32 water_instance = ~0u;
  u32 shores = 0;
};

void shoot(const gfx::Device& device, const test::TempDir& tmp, const std::string& name, f32 wet_m,
           f32 foam_m, u32 shots, Shot* out) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << basin_scene(wet_m, foam_m);
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  // From 14 m up over the basin's south-west rim, looking down across it.
  frame.camera.position = WorldPos{-14.0, 12.0, -14.0};
  frame.camera.target = WorldPos{2.0, -2.0, 2.0};
  frame.sky_time_s = 0.0;
  frame.world_time_s = 3.0;
  CaptureChannels channels;
  channels.ids = true;
  u32 water_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
  }
  for (u32 s = 0; s < shots; ++s) {
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].water_instance = water_instance;
    out[s].shores = scene.water_shore_count();
  }
}

bool is_water(const CapturedFrame& f, u32 p, u32 water_instance) {
  return f.ids[p * k_id_words] == water_instance;
}

u32 luma(const CapturedFrame& f, u32 p) {
  return u32{f.color[p * 4]} + f.color[p * 4 + 1] + f.color[p * 4 + 2];
}

}  // namespace

TEST_CASE("water shore: the wet band darkens the ground at the waterline, and only the ground") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_shore_wet");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Shot dry[1], narrow[2], wide[1];
  shoot(device, tmp, "dry", 0.0f, 0.0f, 1, dry);
  shoot(device, tmp, "narrow", 0.3f, 0.0f, 2, narrow);
  shoot(device, tmp, "wide", 0.6f, 0.0f, 1, wide);
  device.destroy();
  CHECK(dry[0].shores == 0u);
  CHECK(narrow[0].shores == 1u);
  REQUIRE(narrow[0].water_instance != ~0u);
  const CapturedFrame& a = dry[0].frame;
  const CapturedFrame& b = narrow[0].frame;
  const CapturedFrame& c = wide[0].frame;
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(a.ids.size() == b.ids.size());
  CHECK(b.color == narrow[1].frame.color);
  CHECK(a.ids == b.ids);  // the band is shading: it moves no surface
  const u32 pixels = a.width * a.height;
  u32 water = 0, wet_narrow = 0, wet_wide = 0, wet_on_water = 0, lighter = 0;
  for (u32 p = 0; p < pixels; ++p) {
    const bool w = is_water(a, p, narrow[0].water_instance);
    water += w ? 1u : 0u;
    const bool nb = luma(a, p) != luma(b, p) || a.color[p * 4] != b.color[p * 4];
    const bool wb = luma(a, p) != luma(c, p) || a.color[p * 4] != c.color[p * 4];
    wet_narrow += nb ? 1u : 0u;
    wet_wide += wb ? 1u : 0u;
    if ((nb || wb) && w) ++wet_on_water;
    if (nb && luma(b, p) > luma(a, p)) ++lighter;
  }
  MESSAGE("basin: " << water << " water pixels of " << pixels << "; the band at 0.3 m changes "
                    << wet_narrow << ", at 0.6 m " << wet_wide);
  CHECK(water > 2000u);
  CHECK(wet_narrow > 100u);
  CHECK(wet_wide > wet_narrow + wet_narrow / 4);
  CHECK(wet_on_water == 0u);
  CHECK(lighter == 0u);
}

TEST_CASE("water shore: the foam line is on the water, and only on the water") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_shore_foam");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Shot calm[1], foam[1];
  shoot(device, tmp, "calm", 0.0f, 0.0f, 1, calm);
  shoot(device, tmp, "foam", 0.0f, 1.0f, 1, foam);
  device.destroy();
  const CapturedFrame& a = calm[0].frame;
  const CapturedFrame& b = foam[0].frame;
  REQUIRE(a.color.size() == b.color.size());
  CHECK(a.ids == b.ids);
  const u32 pixels = a.width * a.height;
  u32 changed = 0, off_water = 0, water = 0;
  for (u32 p = 0; p < pixels; ++p) {
    const bool w = is_water(a, p, calm[0].water_instance);
    water += w ? 1u : 0u;
    const bool d = a.color[p * 4] != b.color[p * 4] || a.color[p * 4 + 1] != b.color[p * 4 + 1] ||
                   a.color[p * 4 + 2] != b.color[p * 4 + 2];
    if (!d) continue;
    ++changed;
    if (!w) ++off_water;
  }
  MESSAGE("foam: " << changed << " pixels changed of the water's " << water);
  CHECK(changed > 50u);
  CHECK(changed < water / 2);  // a line along the edge, not the whole surface
  CHECK(off_water == 0u);
}

#endif
