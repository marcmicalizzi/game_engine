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
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
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

// ---- S102, second pass: one sheet (docs/subsystems/water.md, "One sheet")
// ------------------------

namespace {

// The bay under a swell too long and low to break — it surges up the sand whole, so no foam is
// born or carried and the sheet is water and nothing else — whose water absorbs 0.3 a metre in
// every band and scatters nothing: the sheet's two sides are then the model's two terms alone.
constexpr f64 k_sheet_extinction = 0.3;
constexpr f64 k_sheet_level = -0.8;
std::string sheet_scene() {
  return R"({
  "format": "engine.scene.v1",
  "name": "sheet-test",
  "terrain": {"size": 257, "extent": 128, "seed": 2, "dune_height": 0,
              "basins": [{"center": [150, 0], "radius": 200, "depth": 8}]},
  "water": [{"name": "bay", "level_m": -0.8, "open": true,
             "shape": {"polygon": [[16.9, 0, -100], [-5.3, 0, -60], [-13.8, 0, -30], [-16.5, 0, 0],
                                   [-13.8, 0, 30], [-5.3, 0, 60], [16.9, 0, 100], [80, 0, 100],
                                   [80, 0, -100]]},
             "wind_follow": false, "wind_m_s": 0, "wet_m": 0.3,
             "absorption": [0.3, 0.3, 0.3], "scattering": [0, 0, 0],
             "swell_height_m": 0.15, "swell_period_s": 30, "swell_direction_deg": 180}]
})";
}

constexpr u32 k_sheet_times = 12;
constexpr u32 k_sheet_eyes = 2;
// Low on the sand: looking along the swash from half a metre over it, and across it to the sea
// from a metre and a quarter.
constexpr f64 k_sheet_eye[k_sheet_eyes][3] = {{-19.5, -0.15, -14.0}, {-23.0, 0.7, 2.0}};
constexpr f64 k_sheet_target[k_sheet_eyes][3] = {{-16.9, -0.8, 6.0}, {-12.0, -0.8, 5.0}};

struct SheetShots {
  CapturedFrame frame[k_sheet_eyes][k_sheet_times];
  u32 water_instance = ~0u;
  f32 fov_y = 0.0f;
  bool lifted = false;
};

void shoot_sheet(const gfx::Device& device, const test::TempDir& tmp, const std::string& name,
                 SheetShots& out) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << sheet_scene();
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
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value())
      out.water_instance = i;
  }
  CaptureChannels channels;
  channels.ids = true;
  channels.normals = true;
  for (u32 e = 0; e < k_sheet_eyes; ++e) {
    FrameDesc frame;
    frame.camera.position = WorldPos{k_sheet_eye[e][0], k_sheet_eye[e][1], k_sheet_eye[e][2]};
    frame.camera.target =
        WorldPos{k_sheet_target[e][0], k_sheet_target[e][1], k_sheet_target[e][2]};
    frame.sky_time_s = 0.0;
    out.fov_y = frame.camera.fov_y;
    // Twelve times over one period of the swell on the water's clock (29.3 s): the sheet is on
    // the sand in about half of them, climbing and draining.
    for (u32 s = 0; s < k_sheet_times; ++s) {
      frame.world_time_s = 10.0 + 2.44 * static_cast<f64>(s);
      REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame[e][s], &error), error);
    }
  }
}

// The cosine the eye's ray through pixel p makes with the vertical, pointing down: what a level
// surface's Fresnel is taken at.
f64 sheet_cosine(u32 eye, u32 p, u32 width, u32 height, f32 fov_y) {
  const f64 x = static_cast<f64>(p % width) + 0.5;
  const f64 y = static_cast<f64>(p / width) + 0.5;
  f64 fwd[3];
  for (u32 i = 0; i < 3; ++i)
    fwd[i] = k_sheet_target[eye][i] - k_sheet_eye[eye][i];
  const f64 fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  for (f64& v : fwd)
    v /= fl;
  // look_at's frame: right = forward x up, up' = right x forward.
  f64 right[3] = {-fwd[2], 0.0, fwd[0]};
  const f64 rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
  right[0] /= rl;
  right[2] /= rl;
  const f64 up_y = right[2] * fwd[0] - right[0] * fwd[2];
  const f64 t = std::tan(0.5 * static_cast<f64>(fov_y));
  const f64 sx = (x / width * 2.0 - 1.0) * t * static_cast<f64>(width) / height;
  const f64 sy = (1.0 - y / height * 2.0) * t;
  f64 dir[3];
  const f64 up[3] = {right[1] * fwd[2] - right[2] * fwd[1], up_y,
                     right[0] * fwd[1] - right[1] * fwd[0]};
  for (u32 i = 0; i < 3; ++i)
    dir[i] = fwd[i] + right[i] * sx + up[i] * sy;
  const f64 dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  return std::max(-dir[1] / dl, 1e-4);
}

f64 sheet_fresnel(f64 cosine) {
  const f64 f0 = 0.020059312;  // water.slang's k_water_f0
  const f64 m = 1.0 - cosine;
  return f0 + (1.0 - f0) * m * m * m * m * m;
}

// **The most the picture may step across the mesh's edge inside the sheet**, in codes of one
// channel, derived from what the two sides are (water.slang, `water_sheet_over_ground`; water.md,
// "One sheet"), not measured. Both are L = U (1 - F) + S F: F Schlick's at the level surface's
// cosine, S the stand-in sky (the water's reflection is off and the scene has no sky), U the wet
// ground lit through the surface. So across the edge
//
//   L_w - L_g = (F_w - F_g) (S - U_g) + (1 - F_w) (U_w - U_g),
//
// and: U_g (1 - F_g) <= L_g < 1 for a pixel under white, so |S - U_g| <= max(S, 1 / (1 - F_g));
// U_w differs from U_g by the two bottoms' normals (`bottoms`, below) and by what a film of water
// takes: the water's pixel beside the edge is at most half the ground's rise over its own
// footprint deep (the sheet is half the front's height over the ground; the bowl is nowhere
// steeper than 1:10), and light crosses it down and up at no more than 1 / cos(48.6 degrees) each,
// 3.04 depths in all, at `k_sheet_extinction`. The encode x^(1/2.2) is concave, so a step in light
// is at most its slope at the darker pixel's level times the step; the dither's two noises and the
// store's rounding are a code each.
//
// **The two bottoms' normals** (`sheet_bottoms`): the ground's pixel lights its sand by its own
// shading normal; the water's lights the bottom by the depth map's slope, **and the map is clamped
// at the level over the bank, so there the water's bottom is level** (`water_path_map`: the
// channels' row holds it; said in the hand-back). The sun's term under the surface goes with n . l,
// l the sun refracted (its horizontal part over 1.33), so the two differ by at most the ground's
// own n . l less a level surface's, over the level one's — from the picture's normals channel,
// 8-bit and so within 2 sqrt(3) / 255 once renormalized — and the sky's term not at all.
f64 sheet_bottoms(const CapturedFrame& f, u32 p_ground) {
  const Vec3 sun = sun_direction(k_default_sun_azimuth_deg, k_default_sun_elevation_deg);
  const f64 lx = static_cast<f64>(sun.x) / 1.33;
  const f64 lz = static_cast<f64>(sun.z) / 1.33;
  const f64 ly = std::sqrt(1.0 - lx * lx - lz * lz);
  f64 n[3];
  for (u32 i = 0; i < 3; ++i)
    n[i] = f.normals[p_ground * 3 + i] / 255.0 * 2.0 - 1.0;
  const f64 nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const f64 lit = (n[0] * lx + n[1] * ly + n[2] * lz) / nl;
  const f64 slack = 2.0 * std::sqrt(3.0) / 255.0 * std::sqrt(lx * lx + lz * lz);
  return (std::fabs(lit - ly) + slack) / ly;
}

f64 sheet_step_bound(u32 eye, u32 p_water, u32 p_ground, u32 channel, u32 code_low, u32 width,
                     u32 height, f32 fov_y, f64 bottoms) {
  const f64 cw = sheet_cosine(eye, p_water, width, height, fov_y);
  const f64 cg = sheet_cosine(eye, p_ground, width, height, fov_y);
  const f64 fw = sheet_fresnel(cw);
  const f64 fg = sheet_fresnel(cg);
  const f64 sky = static_cast<f64>(channel == 0 ? k_sky.x : (channel == 1 ? k_sky.y : k_sky.z));
  const f64 eye_height = k_sheet_eye[eye][1] - k_sheet_level;
  const f64 footprint = eye_height / cw * (static_cast<f64>(fov_y) / height) / cw;
  const f64 film = 0.5 * 0.1 * footprint;
  const f64 under = bottoms + k_sheet_extinction * film * 3.04;
  const f64 light =
      std::fabs(fw - fg) * std::max(sky, 1.0 / (1.0 - fg)) + under * (1.0 - fw) / (1.0 - fg);
  const f64 level = std::pow(static_cast<f64>(std::max(code_low, 2u) - 1u) / 255.0, 2.2);
  const f64 slope = std::pow(level, 1.0 / 2.2 - 1.0) / 2.2;
  return 255.0 * slope * light + 2.0;
}

struct RestoreFilm {
  ~RestoreFilm() {
    (void)tunables::apply_overrides("renderer.water.wet_film=true");
    (void)tunables::apply_overrides("renderer.water.reflect=1");
  }
};

// The device of the project's GPU runner without ray queries (the TITAN Xp; renderer_tests.cpp's
// TITAN-Xp-like case has the same rows): the vertex path and the cascaded shadow maps.
gfx::DeviceOptions titan_xp_like() {
  gfx::DeviceOptions options;
  for (const char* row :
       {"VK_EXT_mesh_shader", "VK_KHR_ray_query", "VK_NV_cluster_acceleration_structure",
        "VK_EXT_memory_decompression", "VK_KHR_fragment_shading_rate"}) {
    options.overrides.absent.push_back(row);
  }
  return options;
}

void check_sheet(const gfx::DeviceOptions& options, const std::string& tier) {
  const test::TempDir tmp("water_sheet");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(options, &error), error);
  MESSAGE("sheet on " << tier << ": ray queries "
                      << std::string(device.features().ray_query ? "yes" : "no")
                      << ", mesh shaders "
                      << std::string(device.features().mesh_shader ? "yes" : "no"));
  SheetShots on, off;
  shoot_sheet(device, tmp, "on", on);
  // With the film off (a measurement switch) the ground under the sheet is wet sand and nothing
  // more: which pixels the sheet is drawn on is the difference of the two.
  REQUIRE(tunables::apply_overrides("renderer.water.wet_film=false"));
  shoot_sheet(device, tmp, "off", off);
  REQUIRE(tunables::apply_overrides("renderer.water.wet_film=true"));
  device.destroy();
  REQUIRE(on.water_instance != ~0u);
  CHECK(on.lifted);
  const u32 width = on.frame[0][0].width;
  const u32 height = on.frame[0][0].height;
  for (u32 e = 0; e < k_sheet_eyes; ++e) {
    u32 pairs = 0, over = 0, seen_off = 0, saturated = 0;
    f64 worst_share = 0.0;
    u32 worst_step = 0;
    // **Which frame has the sheet on the sand.** Between sheets the water's edge is the sea's own —
    // the waterline, where one thing ends and wet sand begins, its film drained to what the clock
    // says — and that edge is not the one this case is about. The swell comes straight on to a
    // shore that curves little in the view, so its sheet climbs the whole of it together and is up
    // for half a period: the time at which the body's water covers the most pixels is the sheet
    // near its top, and there every place the water meets the ground is the mesh's edge inside
    // the sheet (the pixel's own front lies beyond the mesh's, `k_water_mesh_inside`).
    u32 top = 0, most = 0;
    for (u32 s = 0; s < k_sheet_times; ++s) {
      u32 count = 0;
      for (u32 p = 0; p < width * height; ++p)
        if (on.frame[e][s].ids[p * k_id_words] == on.water_instance) ++count;
      if (count > most) {
        most = count;
        top = s;
      }
    }
    for (u32 s = top; s <= top; ++s) {
      const CapturedFrame& a = on.frame[e][s];
      const CapturedFrame& b = off.frame[e][s];
      REQUIRE(a.ids == b.ids);  // the film is shading: it moves no surface
      const auto wet_pixel = [&](u32 p) { return a.ids[p * k_id_words] == on.water_instance; };
      const auto sheeted = [&](u32 p) {
        return !wet_pixel(p) && std::memcmp(&a.color[p * 4], &b.color[p * 4], 3) != 0;
      };
      // Every pair of neighbours, side by side or one over the other, of which one is the body's
      // water and the other ground the sheet is drawn on: the mesh's edge inside the sheet.
      const auto pair = [&](u32 p, u32 q) {
        if (wet_pixel(p) == wet_pixel(q)) return;
        const u32 w = wet_pixel(p) ? p : q;
        const u32 g = wet_pixel(p) ? q : p;
        if (!sheeted(g)) return;
        ++pairs;
        const f64 bottoms = sheet_bottoms(a, g);
        bool breaks = false, breaks_off = false;
        for (u32 ch = 0; ch < 3; ++ch) {
          const u32 cw = a.color[w * 4 + ch];
          const u32 cg = a.color[g * 4 + ch];
          if (cw == 255u || cg == 255u) {
            ++saturated;
            continue;
          }
          const u32 step = cw > cg ? cw - cg : cg - cw;
          const f64 bound =
              sheet_step_bound(e, w, g, ch, std::min(cw, cg), width, height, on.fov_y, bottoms);
          worst_share = std::max(worst_share, static_cast<f64>(step) / bound);
          worst_step = std::max(worst_step, step);
          if (static_cast<f64>(step) > bound) breaks = true;
          // The same pair with the sheet off: the edge as it stood before this pass's shading.
          const u32 og = b.color[g * 4 + ch];
          const u32 ow = b.color[w * 4 + ch];
          const u32 step_off = ow > og ? ow - og : og - ow;
          if (og != 255u && ow != 255u &&
              static_cast<f64>(step_off) >
                  sheet_step_bound(e, w, g, ch, std::min(ow, og), width, height, on.fov_y, bottoms))
            breaks_off = true;
        }
        if (breaks && ++over <= 8u)
          MESSAGE("the edge shows at ("
                  << w % width << ", " << w / width << ") against (" << g % width << ", "
                  << g / width << "), time " << s << ": water " << u32{a.color[w * 4]} << " "
                  << u32{a.color[w * 4 + 1]} << " " << u32{a.color[w * 4 + 2]} << ", ground "
                  << u32{a.color[g * 4]} << " " << u32{a.color[g * 4 + 1]} << " "
                  << u32{a.color[g * 4 + 2]} << ", ground with"
                  << " the sheet off " << u32{b.color[g * 4]} << " " << u32{b.color[g * 4 + 1]}
                  << " " << u32{b.color[g * 4 + 2]});
        if (breaks_off) ++seen_off;
      };
      for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
          const u32 p = y * width + x;
          if (x + 1 < width) pair(p, p + 1);
          if (y + 1 < height) pair(p, p + width);
        }
      }
    }
    MESSAGE("sheet on " << tier << ", eye " << e << ": " << pairs
                        << " water/ground neighbours inside the sheet over " << k_sheet_times
                        << " times; the largest step " << worst_step << " codes, " << worst_share
                        << " of its bound; " << over << " over it; with the sheet off " << seen_off
                        << " are; " << saturated << " channel pairs at white, left out");
    CHECK(pairs > 100u);   // the edge is in the picture
    CHECK(over == 0u);     // and is not seen
    CHECK(seen_off > 0u);  // where without the sheet's shading it is
  }
}

}  // namespace

// On the GPU as it is, and — where it has ray queries — again as the GPU runner without them (the
// vertex path, the cascaded shadow maps, the water modules without the ray), so a machine with
// everything sees what the other shadow path does to the edge (S105: the server leg of batch 63
// was the first to).
TEST_CASE("water swash: the sheet on the sand is one surface across the mesh's edge") {
  if (!gpu_here()) return;
  RestoreFilm restore;
  // The sheet mirrors the stand-in sky, the one light the bound knows.
  REQUIRE(tunables::apply_overrides("renderer.water.reflect=0"));
  bool traced = false;
  {
    gfx::Device probe;
    std::string error;
    REQUIRE_MESSAGE(probe.create(gfx::DeviceOptions{}, &error), error);
    traced = probe.features().ray_query;
    probe.destroy();
  }
  check_sheet(gfx::DeviceOptions{}, "this GPU");
  if (traced) check_sheet(titan_xp_like(), "the TITAN Xp's absences");
}

#endif
