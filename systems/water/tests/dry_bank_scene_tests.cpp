// Ground the body does not hold is dry ground (docs/subsystems/water.md, "Where the water is";
// roadmap S107): a polygon whose one side falls short of its level's contour leaves ground under
// the level outside the outline, inside the body's bounds. By the body's definition — the outline
// is what an authored body is — that ground holds no water and no surface is drawn over it, so the
// picture with the body in the scene is the picture without it there, to the codes the two differ
// by on the rest of the dry ground. Drawn on every tier this machine has: the mesh path with traced
// shadows, the vertex pipeline, and the cascaded maps a device without ray queries draws with.
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
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_WATER_TESTS_DUNES

namespace {

// A basin of the renderer's ground (a smooth bowl 4 m deep in a plain, as the shore's cases), and a
// body at 1.5 m below the plain whose outline is a rectangle: tall along z, so its bounding square
// reaches well past its west side, and that side at x = -2, short of where the level meets the
// bowl, and on the side away from the sun (+x, +z), so a ray to the sun from the ground beyond it
// crosses the level over the body. The other three sides overhang the bowl and run under the
// plain, where the ground hides them.
constexpr f64 k_level = -1.5;
constexpr f64 k_west = -2.0;

std::string bank_scene(bool body) {
  char text[1024];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "dry-bank-test",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]}%s
})",
                body ? R"(,
  "water": [{"name": "short", "level_m": -1.5,
             "shape": {"polygon": [[-2, 0, -14], [10, 0, -14], [10, 0, 14], [-2, 0, 14]]},
             "wind_follow": false, "wave_amplitude_m": 0.03, "wave_length_m": 1.5}])"
                     : "");
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

// From over the west bank, looking east and down at the outline's short side.
constexpr f64 k_eye[3] = {-12.0, 5.0, 3.0};
constexpr f64 k_target[3] = {-2.0, -2.5, 0.0};

struct Shot {
  CapturedFrame frame;
  u32 water_instance = ~0u;
  f32 fov_y = 0.0f;
  f32 znear = 0.0f;
  bool drawn = false;
};

void load(const test::TempDir& tmp, const std::string& name, bool body, SceneData& data) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << bank_scene(body);
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
}

void shoot(const gfx::Device& device, const SceneData& data, RasterMode raster, ShadowMode shadows,
           Shot& out) {
  std::string error;
  RenderSettings settings;
  settings.raster = raster;
  settings.shadows = shadows;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) return;
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera.position = WorldPos{k_eye[0], k_eye[1], k_eye[2]};
  frame.camera.target = WorldPos{k_target[0], k_target[1], k_target[2]};
  frame.sky_time_s = 0.0;
  frame.world_time_s = 3.0;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value())
      out.water_instance = i;
  }
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  out.fov_y = frame.camera.fov_y;
  out.znear = frame.camera.znear;
  out.drawn = true;
}

// The point a pixel's depth names, from the camera's frame (look_at's: right = forward x up,
// up' = right x forward) and the reversed-Z depth's znear / distance along the view axis.
bool point_of(const Shot& shot, u32 p, f64 out[3]) {
  const CapturedFrame& f = shot.frame;
  const f32 depth = f.depth[p];
  if (!(depth > 0.0f)) return false;
  f64 fwd[3] = {k_target[0] - k_eye[0], k_target[1] - k_eye[1], k_target[2] - k_eye[2]};
  const f64 fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  for (f64& v : fwd)
    v /= fl;
  f64 right[3] = {-fwd[2], 0.0, fwd[0]};
  const f64 rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
  right[0] /= rl;
  right[2] /= rl;
  const f64 up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                     right[0] * fwd[1] - right[1] * fwd[0]};
  const f64 t = std::tan(0.5 * static_cast<f64>(shot.fov_y));
  const f64 x = static_cast<f64>(p % f.width) + 0.5;
  const f64 y = static_cast<f64>(p / f.width) + 0.5;
  const f64 sx = (x / f.width * 2.0 - 1.0) * t * static_cast<f64>(f.width) / f.height;
  const f64 sy = (1.0 - y / f.height * 2.0) * t;
  const f64 along = static_cast<f64>(shot.znear) / static_cast<f64>(depth);
  for (u32 i = 0; i < 3; ++i)
    out[i] = k_eye[i] + along * (fwd[i] + right[i] * sx + up[i] * sy);
  return true;
}

u32 channel_step(const CapturedFrame& a, const CapturedFrame& b, u32 p) {
  u32 worst = 0;
  for (u32 c = 0; c < 3; ++c) {
    const u32 x = a.color[p * 4 + c];
    const u32 y = b.color[p * 4 + c];
    worst = std::max(worst, x > y ? x - y : y - x);
  }
  return worst;
}

}  // namespace

TEST_CASE("water dry bank: ground outside an outline is dry ground, whatever its height") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_dry_bank");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  SceneData with_body, without_body;
  load(tmp, "with", true, with_body);
  load(tmp, "without", false, without_body);
  // The bounce (`renderer.gi.mode`) is left out: its picture is not the same bytes twice (which
  // pixels a frame's rays go to is the order threads reach an atomic), so it has no exact control.
  // Measured by hand with it on, the 2026-11-14 run: on 61ccce67 43,295 of 46,849 dry pixels under
  // the level changed, by up to 82 codes; on this branch 1,223, by 1, as the dry ground above did.
  struct Tier {
    const char* name;
    RasterMode raster;
    ShadowMode shadows;
  };
  const Tier tiers[] = {{"mesh, traced shadows", RasterMode::Hardware, ShadowMode::Auto},
                        {"vertex pipeline", RasterMode::Vertex, ShadowMode::Auto},
                        {"cascaded maps", RasterMode::Hardware, ShadowMode::Cascaded}};
  u32 tiers_drawn = 0;
  for (const Tier& tier : tiers) {
    Shot with, without;
    shoot(device, with_body, tier.raster, tier.shadows, with);
    shoot(device, without_body, tier.raster, tier.shadows, without);
    if (!with.drawn || !without.drawn) {
      MESSAGE(std::string(tier.name) << ": not available on this device");
      continue;
    }
    ++tiers_drawn;
    const CapturedFrame& a = with.frame;
    const CapturedFrame& b = without.frame;
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(with.water_instance != ~0u);
    const u32 pixels = a.width * a.height;
    // Dry by the body's definition: outside its outline (5 cm clear of the side, so a pixel is not
    // the side's own), not the body's surface, and the same ground in both pictures. Those under
    // the level are the case; those above the wet band (`wet_m`, 0.3 m by default: the fringe
    // the shore draws beside a body, water.md "The shore") are what the two pictures differ by on
    // dry ground anyway.
    u32 under = 0, under_changed = 0, under_worst = 0;
    u32 above = 0, above_worst = 0, water = 0;
    for (u32 p = 0; p < pixels; ++p) {
      if (a.ids[p * k_id_words] == with.water_instance) {
        ++water;
        continue;
      }
      // The same surface in both (the body's mesh comes before the terrain's, so the ground's
      // instance index is not the same number in the two scenes; its depth is the same).
      if (a.depth[p] != b.depth[p]) continue;
      f64 q[3];
      if (!point_of(with, p, q)) continue;
      if (!(q[0] < k_west - 0.05)) continue;
      const u32 step = channel_step(a, b, p);
      if (q[1] < k_level - 0.02) {
        ++under;
        under_changed += step != 0 ? 1u : 0u;
        under_worst = std::max(under_worst, step);
      } else if (q[1] > k_level + 0.3 + 0.05) {
        ++above;
        above_worst = std::max(above_worst, step);
      }
    }
    MESSAGE(std::string(tier.name) << ": " << water << " water pixels; " << under
                      << " dry pixels under the level, " << under_changed << " of them changed, by "
                      << under_worst << " codes at worst; " << above
                      << " dry pixels above the wet band, by " << above_worst << " at worst");
    if (under_worst > above_worst) {
      tmp.keep();
      CaptureFiles files;
      std::string why;
      (void)write_capture(tmp.path(), std::string("with-") + std::to_string(tiers_drawn), a,
                          files, why);
      (void)write_capture(tmp.path(), std::string("without-") + std::to_string(tiers_drawn), b,
                          files, why);
    }
    CHECK(water > 1000u);
    CHECK(under > 500u);  // the case has ground under the level outside the outline in view
    CHECK(above > 500u);
    CHECK(under_worst <= above_worst);
  }
  device.destroy();
  CHECK(tiers_drawn >= 1u);
}

#endif  // ENGINE_WATER_TESTS_DUNES
