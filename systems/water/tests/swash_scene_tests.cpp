// The swash on a GPU (docs/subsystems/water.md, "The swash"; roadmap S94): a body that fills a
// basin of the renderer's ground, drawn through the renderer's reader and the resolve's water
// modules, over a sequence of world times. With a wind the wet band's extent changes from frame to
// frame, rising when a sheet runs up and falling between, and no surface moves; the same time twice
// is the same bytes; and with no wind and no tide the ground is the same at every time (version 4's
// static band), only the water's waves moving.
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

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_WATER_TESTS_DUNES

namespace {

// The S97 basin (shore_scene_tests.cpp), its body open where it has a wind so its swash takes
// `fetch_km` (a closed basin of 28 m has a fetch of 40 m and centimetre waves), with a wet band.
std::string swash_scene(f32 wind, f32 wet_m, bool open) {
  char text[1024];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "swash-test",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]},
  "water": [{"name": "basin", "level_m": -1.5, "open": %s,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 30}},
             "wind_follow": false, "wave_amplitude_m": 0.03, "wave_length_m": 1.5,
             "wind_m_s": %g, "fetch_km": 2, "wet_m": %g, "foam_m": 0.6}]
})",
                open ? "true" : "false", static_cast<f64>(wind), static_cast<f64>(wet_m));
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

constexpr u32 k_times = 8;

struct Sequence {
  CapturedFrame frame[k_times + 1];  // the last: the first time again
  u32 water_instance = ~0u;
};

void shoot(const gfx::Device& device, const test::TempDir& tmp, const std::string& name, f32 wind,
           f32 wet_m, bool open, Sequence& out) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << swash_scene(wind, wet_m, open);
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
  // Low over the basin's south-west rim, looking across at the lit north-east bank.
  frame.camera.position = WorldPos{-6.0, 1.0, -6.0};
  frame.camera.target = WorldPos{8.0, -1.5, 8.0};
  frame.sky_time_s = 0.0;
  CaptureChannels channels;
  channels.ids = true;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value())
      out.water_instance = i;
  }
  for (u32 s = 0; s <= k_times; ++s) {
    frame.world_time_s = 10.0 + 0.25 * static_cast<f64>(s == k_times ? 0 : s);
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame[s], &error), error);
  }
}

u32 luma(const CapturedFrame& f, u32 p) {
  return u32{f.color[p * 4]} + f.color[p * 4 + 1] + f.color[p * 4 + 2];
}

}  // namespace

TEST_CASE("water swash: the wet band runs up and back with the clock, and only the shading moves") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_swash");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Sequence dry, wind, still;
  shoot(device, tmp, "dry", 0.0f, 0.0f, true, dry);       // no record: the plain ground
  shoot(device, tmp, "wind", 6.0f, 0.3f, true, wind);     // the swash
  shoot(device, tmp, "still", 0.0f, 0.3f, false, still);  // version 4's static band
  device.destroy();
  const u32 pixels = dry.frame[0].width * dry.frame[0].height;
  REQUIRE(wind.water_instance != ~0u);
  // The same time twice is the same bytes; no surface moves.
  CHECK(wind.frame[0].color == wind.frame[k_times].color);
  for (u32 s = 0; s < k_times; ++s) {
    CHECK(wind.frame[s].ids == dry.frame[s].ids);
  }
  // The wet ground (darker than the dry ground at the same time, off the water) at each time.
  u32 wet[k_times] = {};
  u32 still_ground_moved = 0;
  for (u32 s = 0; s < k_times; ++s) {
    for (u32 p = 0; p < pixels; ++p) {
      if (dry.frame[s].ids[p * k_id_words] == wind.water_instance) continue;
      if (luma(wind.frame[s], p) + 3 < luma(dry.frame[s], p)) ++wet[s];
      if (s > 0 && luma(still.frame[s], p) != luma(still.frame[0], p)) ++still_ground_moved;
    }
  }
  u32 lo = ~0u, hi = 0, rises = 0, falls = 0;
  for (u32 s = 0; s < k_times; ++s) {
    lo = std::min(lo, wet[s]);
    hi = std::max(hi, wet[s]);
    if (s > 0 && wet[s] > wet[s - 1]) ++rises;
    if (s > 0 && wet[s] < wet[s - 1]) ++falls;
  }
  MESSAGE("swash: wet ground pixels over 2 s: "
          << wet[0] << " " << wet[1] << " " << wet[2] << " " << wet[3] << " " << wet[4] << " "
          << wet[5] << " " << wet[6] << " " << wet[7] << "; still band's ground moved "
          << still_ground_moved);
  CHECK(lo > 50u);
  CHECK(hi > lo + lo / 20);  // the band's extent moves
  CHECK(rises > 0u);
  CHECK(falls > 0u);
  // No wind, no tide: the ground is the same at every time (only the water's waves move).
  CHECK(still_ground_moved == 0u);
}

#endif
