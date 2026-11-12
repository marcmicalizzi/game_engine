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
  bool lifted = false;  // the GPU scene gave the body a water-stage deform record (S100)
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
  out.lifted = scene.water_lifted();
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

// Since S100 the windy body's mesh moves (the pool lifts it by its height function, water_waves.
// slang), so its waterline is in the ids channel: S94's "no surface moves" (the ids the dry frame's
// at every time) went with the flat mesh it described, and in its place the ids change only where
// the water is in one of the two frames, the waterline moves between the times, and the still
// body's ids stand.
TEST_CASE("water swash: the wet band runs up and back with the clock, and the waterline with it") {
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
  // A body with no wind and no tide takes no deform record: it is the rigid instance it was, S97's
  // bytes by construction. The windy one is lifted by the pool.
  CHECK(wind.lifted);
  CHECK_FALSE(still.lifted);
  CHECK_FALSE(dry.lifted);
  // The same time twice is the same bytes.
  CHECK(wind.frame[0].color == wind.frame[k_times].color);
  CHECK(wind.frame[0].ids == wind.frame[k_times].ids);
  // Only the water moves: a pixel whose id is not the dry frame's is the water's in one of them;
  // and the waterline moves from one time to the next. The still body's ids stand.
  u32 not_water = 0, waterline_moved = 0;
  for (u32 s = 0; s < k_times; ++s) {
    CHECK(still.frame[s].ids == still.frame[0].ids);
    for (u32 p = 0; p < pixels; ++p) {
      const u32 a = wind.frame[s].ids[p * k_id_words];
      const u32 b = dry.frame[s].ids[p * k_id_words];
      if (a != b && a != wind.water_instance && b != wind.water_instance) ++not_water;
      if (s > 0 && (a == wind.water_instance) !=
                       (wind.frame[s - 1].ids[p * k_id_words] == wind.water_instance))
        ++waterline_moved;
    }
  }
  CHECK(not_water == 0u);
  CHECK(waterline_moved > 0u);
  MESSAGE("swash: " << waterline_moved << " water/ground id changes over 2 s at a quarter second");
  // The wet ground (darker than the dry ground at the same time, off the water) at each time.
  u32 wet[k_times] = {};
  u32 still_ground_moved = 0;
  for (u32 s = 0; s < k_times; ++s) {
    for (u32 p = 0; p < pixels; ++p) {
      if (dry.frame[s].ids[p * k_id_words] == wind.water_instance) continue;
      if (wind.frame[s].ids[p * k_id_words] == wind.water_instance) continue;
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

// ---- S102: a sea that comes in at an angle, on a bay (docs/experiments/shore-mechanics.md)
// -------

namespace {

// A bay cut by one wide basin: the shore an arc facing east, about 1:24 at the waterline, an open
// sea whose outline follows it. `extra` is appended to the body (a swell's fields).
std::string bay_scene(const char* extra) {
  char text[2048];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "bay-test",
  "terrain": {"size": 257, "extent": 128, "seed": 2, "dune_height": 0,
              "basins": [{"center": [150, 0], "radius": 200, "depth": 8}]},
  "water": [{"name": "bay", "level_m": -0.8, "open": true,
             "shape": {"polygon": [[16.9, 0, -100], [-5.3, 0, -60], [-13.8, 0, -30], [-16.5, 0, 0],
                                   [-13.8, 0, 30], [-5.3, 0, 60], [16.9, 0, 100], [80, 0, 100],
                                   [80, 0, -100]]},
             "wind_follow": false, "direction_deg": 200, "wind_m_s": 8, "fetch_km": 60,
             "wet_m": 0.3%s}]
})",
                extra);
  return text;
}

struct BayShots {
  CapturedFrame at_10;
  CapturedFrame again;
  CapturedFrame wrapped;  // 1,024 s later: the water's clock has gone round once
  CapturedFrame later;    // a second on
  bool lifted = false;
  u32 water_instance = ~0u;
};

void shoot_bay(const gfx::Device& device, const test::TempDir& tmp, const std::string& name,
               const char* extra, BayShots& out) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << bay_scene(extra);
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
  out.lifted = scene.water_lifted();
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  // Thirty metres over the beach, looking out and down at the surf and the sand below it.
  frame.camera.position = WorldPos{-40.0, 30.0, 0.0};
  frame.camera.target = WorldPos{15.0, -0.8, 0.0};
  frame.sky_time_s = 0.0;
  CaptureChannels channels;
  channels.ids = true;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value())
      out.water_instance = i;
  }
  frame.world_time_s = 10.0;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.at_10, &error), error);
  frame.world_time_s = 11.0;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.later, &error), error);
  frame.world_time_s = 10.0 + 1024.0;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.wrapped, &error), error);
  frame.world_time_s = 10.0;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.again, &error), error);
}

}  // namespace

TEST_CASE("water swash: a sea that comes in at an angle is a function of the clock, and wraps") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_bay");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  BayShots sea, swell;
  shoot_bay(device, tmp, "sea", "", sea);
  shoot_bay(device, tmp, "swell",
            R"(, "swell_height_m": 0.8, "swell_period_s": 10, "swell_direction_deg": 165)", swell);
  device.destroy();
  for (const BayShots* shots : {&sea, &swell}) {
    REQUIRE(shots->water_instance != ~0u);
    CHECK(shots->lifted);
    // The same time twice, with other times drawn between: the same bytes (nothing is kept).
    CHECK(shots->at_10.color == shots->again.color);
    CHECK(shots->at_10.ids == shots->again.ids);
    // The water's clock wraps every 1,024 s with no jump: the overlapping sheets, the train from
    // its direction and the swell are all whole numbers of cycles of it.
    CHECK(shots->at_10.color == shots->wrapped.color);
    CHECK(shots->at_10.ids == shots->wrapped.ids);
    // And it moves: a second on is another picture.
    CHECK(shots->at_10.color != shots->later.color);
  }
  // The swell is another sea: the same clock draws another surface and another waterline.
  const u32 pixels = sea.at_10.width * sea.at_10.height;
  u32 differ = 0, waterline = 0;
  for (u32 p = 0; p < pixels; ++p) {
    if (luma(sea.at_10, p) != luma(swell.at_10, p)) ++differ;
    if ((sea.at_10.ids[p * k_id_words] == sea.water_instance) !=
        (swell.at_10.ids[p * k_id_words] == swell.water_instance))
      ++waterline;
  }
  MESSAGE("bay: " << differ << " of " << pixels << " pixels differ with a 0.8 m, 10 s swell, "
                  << waterline << " of them between water and ground");
  CHECK(differ > pixels / 20);
  CHECK(waterline > 0u);
}

#endif
