// The medium on a GPU (docs/subsystems/volumetrics.md, "Testing"), offscreen: a frame with the
// medium differs from the frame without it, the same frame is the same bytes twice (no history),
// the frame after the pass is gone is the frame before it to the byte, and the frames after the
// first allocate nothing.

#include <core/json/json.h>
#include <core/memory/memory.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/volumetrics/volumetrics.h>

#include <doctest/doctest.h>

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
    RenderSettings settings;
    settings.shadows = shadows;
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
