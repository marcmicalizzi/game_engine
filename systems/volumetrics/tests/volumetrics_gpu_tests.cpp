// The medium on a GPU (docs/subsystems/volumetrics.md, "Testing"), offscreen: a frame with the
// medium differs from the frame without it, the same frame is the same bytes twice (no history),
// the frame after the pass is gone is the frame before it to the byte, and the frames after the
// first allocate nothing.

#include <core/json/json.h>
#include <core/memory/memory.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/fire.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/volumetrics/volumetrics.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace engine;
using namespace engine::renderer;
using namespace engine::volumetrics;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
};

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;
  const FireDesc* fire = nullptr;  // a fire on the scene (R85)
  bool lights = true;              // the stand-in's two point lights

  bool build(const gfx::Device& device, bool sky, ShadowMode shadows) {
    SceneDesc desc;
    desc.meshes.push_back("");
    desc.heightfield_grid = 65;
    desc.cache = false;
#if ENGINE_VOLUMETRICS_TESTS_SKY
    if (sky) {
      scene::Sky entry;
      entry.latitude_deg = 31.1f;
      entry.day_of_year = 100.0f;
      entry.turbidity = 2.5f;
      desc.sky = entry;
    }
#else
    (void)sky;
#endif
    if (!load_scene(desc, data, error)) return false;
    if (fire != nullptr) data.fires.push_back(*fire);  // R85: before the renderer takes the scene
    RenderSettings settings;
    settings.shadows = shadows;
    settings.lights = lights;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

FrameDesc looking(f64 time_s) {
  FrameDesc frame;
  const Vec3 eye{0.0f, 2.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.08f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.sun_time_s = time_s;
  return frame;
}

bool shoot(Rig& rig, const FrameDesc& frame, CapturedFrame& out) {
  CaptureChannels channels;
  channels.color = true;
  return rig.renderer.capture(frame, channels, out, &rig.error);
}

u32 differing(const CapturedFrame& a, const CapturedFrame& b) {
  u32 n = 0;
  for (u32 i = 0; i < a.color.size(); ++i)
    n += a.color[i] != b.color[i] ? 1u : 0u;
  return n;
}

void hold(bool sky, ShadowMode shadows, f64 time_s) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, sky, shadows), rig.error);
  const FrameDesc frame = looking(time_s);
  CapturedFrame bare;
  REQUIRE_MESSAGE(shoot(rig, frame, bare), rig.error);

  MediumDesc desc;
  desc.dust_scale = 20.0f;  // a thick haze over a small scene, so every pixel says it is there
  MediumPass pass;
  REQUIRE_MESSAGE(pass.create(gpu.device, rig.renderer, rig.data, desc, {}, &rig.error), rig.error);
  CapturedFrame first;
  CapturedFrame second;
  REQUIRE_MESSAGE(shoot(rig, frame, first), rig.error);
  REQUIRE_MESSAGE(shoot(rig, frame, second), rig.error);
  const u32 changed = differing(bare, first);
  MESSAGE("the medium changed " << changed << " of " << bare.color.size()
                                << " channels; visibility " << pass.stats().visibility_m << " m");
  CHECK(changed * 2 > bare.color.size());
  CHECK(first.color == second.color);
  CHECK(pass.stats().active);
  CHECK(pass.stats().visibility_m > 0.0f);
  CHECK(rig.renderer.stats().visibility_m == pass.stats().visibility_m);

  // After the first frames, a frame allocates nothing: the blocks and the grids are the first
  // frame's (renderer.md, "The frame loop allocates nothing").
  for (u32 f = 0; f < 3; ++f)
    REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
  const u64 allocations = mem::allocations_on_thread();
  const u64 buffers = gfx::buffers_created_on_thread();
  for (u32 f = 0; f < 4; ++f) {
    rig.renderer.begin_frame();
    REQUIRE_MESSAGE(rig.renderer.submit_frame(frame, &rig.error) != 0, rig.error);
  }
  CHECK(gfx::buffers_created_on_thread() == buffers);
  CHECK(mem::allocations_on_thread() == allocations);
  rig.renderer.wait_idle();

  pass.destroy();
  CapturedFrame after;
  REQUIRE_MESSAGE(shoot(rig, frame, after), rig.error);
  CHECK(after.color == bare.color);
}

}  // namespace

TEST_CASE("volumetrics: the medium over the stand-in sky, the same bytes twice and none after") {
  hold(false, ShadowMode::Off, 0.0);
}

#if ENGINE_VOLUMETRICS_TESTS_SKY
TEST_CASE("volumetrics: the medium under a sky and the cascaded maps, the same bytes twice") {
  hold(true, ShadowMode::Cascaded, 16.0 * 3600.0);
}
#endif

// ---- fire (R85; volumetrics.md, "Fire")
// ----------------------------------------------------------

TEST_CASE("volumetrics: the smoke leaves the flame's top and drifts downwind as it rises") {
  renderer::FrameFire fire;
  fire.base = WorldPos{10.0, 0.0, 0.0};
  fire.radius_m = 0.3f;
  fire.height_m = 0.8f;
  fire.smoke_density = 1.0f;
  fire.smoke_rise_m_s = 1.0f;
  fire.smoke_spread = 0.2f;
  MediumWind wind;  // the record's mean, towards +x: 8 m/s, half of it at a fire's height
  const MediumFire f = medium_fire(fire, WorldPos{0.0, 0.0, 0.0}, wind, 0.0, false);
  // At the top of the flame, over it; two metres up, the centre has drifted 2 * 4 / 1 = 8 m
  // downwind, and is thinner there than at the top.
  const f32 top = smoke_density(f, Vec3{10.0f, 0.8f, 0.0f});
  const f32 over = smoke_density(f, Vec3{10.0f, 2.8f, 0.0f});
  const f32 downwind = smoke_density(f, Vec3{18.0f, 2.8f, 0.0f});
  const f32 upwind = smoke_density(f, Vec3{2.0f, 2.8f, 0.0f});
  MESSAGE("top " << top << ", 2 m over the flame " << over << ", 8 m downwind " << downwind);
  CHECK(top == doctest::Approx(static_cast<f64>(k_smoke_extinction_per_m)));
  CHECK(downwind > 0.0f);
  CHECK(downwind < top);
  CHECK(downwind > 100.0f * over);
  CHECK(upwind == 0.0f);
  CHECK(smoke_density(f, Vec3{10.0f, -1.0f, 0.0f}) == 0.0f);  // nothing below the flame
  // No smoke, none anywhere.
  renderer::FrameFire clean = fire;
  clean.smoke_density = 0.0f;
  CHECK(smoke_density(medium_fire(clean, WorldPos{}, wind, 0.0, false), Vec3{10.0f, 0.8f, 0.0f}) ==
        0.0f);
  // The flame's noise rises with the clock, reduced to its period: finite years in.
  const MediumFire late = medium_fire(fire, WorldPos{}, wind, 94687200.0, false);
  CHECK(late.flame.z >= 0.0f);
  CHECK(late.flame.z < k_flame_noise_period);
}

namespace {

struct FireView {
  FrameDesc frame;
  f64 distance = 0.0;  // the eye to the fire's light
  WorldPos light{};
};

// The camera `distance` from the fire's light and four metres over it, looking at it, so the light
// is the picture's centre and a pixel's angle from it is the angle of its ray from the light's.
FireView look_at_fire(const SceneData& probe, const FireDesc& fire, f64 horizontal) {
  FireView v;
  v.light = fire.position + DVec3{0.0, static_cast<f64>(k_fire_light_height * fire.height_m), 0.0};
  (void)probe;
  v.frame.camera.position = v.light + DVec3{0.0, 4.0, horizontal};
  v.frame.camera.target = v.light;
  v.frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  v.frame.camera.znear = 0.05f;
  v.distance = std::sqrt(16.0 + horizontal * horizontal);
  return v;
}

// The pixel's angle from the picture's centre (the view's axis), radians.
f64 off_axis(u32 x, u32 y) {
  const f64 t = std::tan(0.5 * 60.0 * 3.14159265358979 / 180.0);
  const f64 nx = ((x + 0.5) / k_width * 2.0 - 1.0) * t * static_cast<f64>(k_width) / k_height;
  const f64 ny = (1.0 - (y + 0.5) / k_height * 2.0) * t;
  return std::atan(std::sqrt(nx * nx + ny * ny));
}

struct Changes {
  u32 inside = 0;
  u32 outside = 0;
  f64 sum_x = 0.0;
};
Changes changes(const CapturedFrame& a, const CapturedFrame& b, f64 limit) {
  Changes c;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 i = (y * k_width + x) * 4;
      if (a.color[i] == b.color[i] && a.color[i + 1] == b.color[i + 1] &&
          a.color[i + 2] == b.color[i + 2])
        continue;
      (off_axis(x, y) <= limit ? c.inside : c.outside)++;
      c.sum_x += x;
    }
  }
  return c;
}

// One capture with a medium pass made for it and gone after it.
bool shoot_medium(Rig& rig, const gfx::Device& device, const MediumDesc& desc,
                  const MediumOptions& options, const FrameDesc& frame, CapturedFrame& out,
                  MediumStats* stats = nullptr) {
  MediumPass pass;
  if (!pass.create(device, rig.renderer, rig.data, desc, options, &rig.error)) return false;
  const bool ok = shoot(rig, frame, out);
  if (stats != nullptr) *stats = pass.stats();
  pass.destroy();
  return ok;
}

}  // namespace

TEST_CASE("volumetrics: a fire's flame and glow are where the fire is and nowhere else") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  // A small fire on the heightfield, its light reaching 2.8 m (the stand-in's units), no smoke, no
  // flicker, in a thick haze: what the medium adds for it is its flame and the haze it lights.
  Rig probe;
  REQUIRE_MESSAGE(probe.build(gpu.device, false, ShadowMode::Off), probe.error);
  FireDesc fire;
  fire.position = probe.data.center + DVec3{0.0, 0.1 * static_cast<f64>(probe.data.radius), 0.0};
  fire.radius_m = 0.3f;
  fire.height_m = 0.8f;
  fire.intensity = 0.4f;
  fire.flicker = 0.0f;
  fire.smoke_density = 0.0f;
  Rig rig;
  rig.fire = &fire;
  rig.lights = false;
  REQUIRE_MESSAGE(rig.build(gpu.device, false, ShadowMode::Off), rig.error);
  MediumDesc desc;
  desc.dust_scale = 20.0f;
  MediumOptions without;
  without.fires = false;

  // Past the near distance: the flame is the froxels'.
  {
    const FireView view = look_at_fire(rig.data, fire, 10.0);
    CapturedFrame on, off, again;
    MediumStats stats;
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, {}, view.frame, on, &stats), rig.error);
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, without, view.frame, off), rig.error);
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, {}, view.frame, again), rig.error);
    CHECK(stats.fires == 1);
    CHECK(stats.near_fires == 0);
    CHECK(on.color == again.color);  // the same clock, the same bytes
    // Inside the light's reach seen from the eye, and a froxel column and a half for the apply's
    // bilinear lookup; nothing outside.
    const f64 reach = static_cast<f64>(fire_light_reach(fire.intensity));
    const f64 column = 2.0 *
                       std::atan(std::tan(0.5 * 60.0 * 3.14159265358979 / 180.0) *
                                 static_cast<f64>(k_width) / k_height) /
                       160.0;
    const f64 limit = std::asin(std::min(1.0, reach / view.distance)) + 1.5 * column;
    const Changes c = changes(on, off, limit);
    MESSAGE("far: the fire changed " << c.inside << " pixels inside " << limit * 180.0 / 3.14159265
                                     << " degrees of it and " << c.outside << " outside");
    CHECK(c.inside > 200);
    CHECK(c.outside == 0);
    if (c.outside != 0) MESSAGE("the light's reach is " << reach << " m at " << view.distance);
    // Another clock, another flame: the noise rises.
    FrameDesc later = view.frame;
    later.sun_time_s = 0.5;
    CapturedFrame moved;
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, {}, later, moved), rig.error);
    CHECK(changes(on, moved, limit).inside > 0);
  }
  // Within it: the apply marches the flame per pixel, and its centre glows hotter in red than blue.
  {
    const FireView view = look_at_fire(rig.data, fire, 2.0);
    CapturedFrame on, off;
    MediumStats stats;
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, {}, view.frame, on, &stats), rig.error);
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, without, view.frame, off), rig.error);
    CHECK(stats.near_fires == 1);
    const u32 centre = (k_height / 2 * k_width + k_width / 2) * 4;
    MESSAGE("near: the centre is " << u32{on.color[centre]} << ", " << u32{on.color[centre + 1]}
                                   << ", " << u32{on.color[centre + 2]} << " with the fire and "
                                   << u32{off.color[centre]} << ", " << u32{off.color[centre + 1]}
                                   << ", " << u32{off.color[centre + 2]} << " without");
    CHECK(on.color[centre] > off.color[centre]);
    CHECK(on.color[centre] > on.color[centre + 2]);
  }
  // No fire at all: the pass with fires on draws the bytes it draws with them off.
  {
    const FireView view = look_at_fire(probe.data, fire, 10.0);
    CapturedFrame on, off;
    REQUIRE_MESSAGE(shoot_medium(probe, gpu.device, desc, {}, view.frame, on), probe.error);
    REQUIRE_MESSAGE(shoot_medium(probe, gpu.device, desc, without, view.frame, off), probe.error);
    CHECK(on.color == off.color);
  }
  // A frame with a fire allocates nothing after the first.
  {
    const FireView view = look_at_fire(rig.data, fire, 2.0);
    MediumPass pass;
    REQUIRE_MESSAGE(pass.create(gpu.device, rig.renderer, rig.data, desc, {}, &rig.error),
                    rig.error);
    for (u32 f = 0; f < 3; ++f)
      REQUIRE_MESSAGE(rig.renderer.render_offscreen(view.frame, &rig.error), rig.error);
    const u64 allocations = mem::allocations_on_thread();
    const u64 buffers = gfx::buffers_created_on_thread();
    for (u32 f = 0; f < 4; ++f) {
      rig.renderer.begin_frame();
      REQUIRE_MESSAGE(rig.renderer.submit_frame(view.frame, &rig.error) != 0, rig.error);
    }
    CHECK(gfx::buffers_created_on_thread() == buffers);
    CHECK(mem::allocations_on_thread() == allocations);
    rig.renderer.wait_idle();
  }
}

TEST_CASE("volumetrics: a fire's smoke drifts downwind") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig probe;
  REQUIRE_MESSAGE(probe.build(gpu.device, false, ShadowMode::Off), probe.error);
  // Smoke and no light: the column the stand-in's sun and sky light, nothing the fire lights.
  FireDesc fire;
  fire.position = probe.data.center + DVec3{0.0, 0.1 * static_cast<f64>(probe.data.radius), 0.0};
  fire.intensity = 0.0f;
  fire.smoke_density = 1.0f;
  fire.smoke_rise_m_s = 1.0f;
  Rig rig;
  rig.fire = &fire;
  rig.lights = false;
  REQUIRE_MESSAGE(rig.build(gpu.device, false, ShadowMode::Off), rig.error);
  MediumDesc desc;
  MediumOptions without;
  without.fires = false;
  const FireView view = look_at_fire(rig.data, fire, 10.0);
  CapturedFrame on, off;
  REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, {}, view.frame, on), rig.error);
  REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, without, view.frame, off), rig.error);
  const Changes c = changes(on, off, 10.0);
  const u32 n = c.inside + c.outside;
  REQUIRE(n > 100);
  // The camera looks along -z, so the record's wind towards +x is to the right of the picture.
  const f64 mean_x = c.sum_x / n;
  MESSAGE("the smoke changed " << n << " pixels, their mean column " << mean_x << " of "
                               << k_width);
  CHECK(mean_x > 0.6 * k_width);
}
