// The frame-pass registration point (frame_pass.h; docs/subsystems/renderer.md, "Frame passes"):
// with no pass registered the frame is the bytes it was, a pass that changes nothing draws the
// resolve's picture to the half's rounding, its context says what the frame is, a debug view never
// calls it, and removing the last pass gives the frame back to the byte.

#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/frame_pass.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#if ENGINE_FRAME_PASS_TESTS_SKY
#include <domain/scene_gen/scene_gen.h>
#endif

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

using namespace engine;
using namespace engine::renderer;

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

SceneDesc hills(bool sky) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  if (sky) {
    scene::Sky entry;
    entry.latitude_deg = 31.1f;
    entry.day_of_year = 100.0f;
    entry.turbidity = 1.6f;
    desc.sky = entry;
  }
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, bool sky) {
    if (!load_scene(hills(sky), data, error)) return false;
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
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

// Low over the hills towards the horizon, so the frame has ground and an uncovered sky.
FrameDesc looking(f64 time_s, u32 view_mode = ~u32{0}) {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.sun_time_s = time_s;
  frame.view_mode = view_mode;
  return frame;
}

// What the null pass saw of the frame, and how often it was called.
struct Seen {
  u32 calls = 0;
  u32 views = 0;
  u32 width = 0;
  u32 height = 0;
  bool color = false;
  bool visibility = false;
  bool storage = false;
  u64 sky_params = 0;
  f64 dt_s = 0.0;
  f32 view_width = 0.0f;
};

bool null_pass(const FramePassContext& context, void* user, std::string*) {
  Seen& seen = *static_cast<Seen*>(user);
  ++seen.calls;
  seen.views = context.view_count;
  seen.width = context.width;
  seen.height = context.height;
  seen.color = context.color.valid();
  seen.visibility = context.visibility.valid();
  seen.storage = context.color_storage != gfx::BindlessSet::k_invalid_slot;
  seen.sky_params = context.sky_params;
  seen.dt_s = context.dt_s;
  seen.view_width = context.view_count > 0 ? static_cast<f32>(context.views[0].width) : 0.0f;
  context.report->visibility_m = 1234.0f;
  return true;
}

bool shoot(Rig& rig, const FrameDesc& frame, CapturedFrame& out) {
  CaptureChannels channels;
  channels.color = true;
  return rig.renderer.capture(frame, channels, out, &rig.error);
}

void hold(bool sky, f64 time_s) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, sky), rig.error);
  FrameDesc frame = looking(time_s);
  frame.dt_s = 1.0 / 60.0;

  CapturedFrame before;
  REQUIRE_MESSAGE(shoot(rig, frame, before), rig.error);
  CHECK(rig.renderer.stats().visibility_m < 0.0f);

  Seen seen;
  REQUIRE_MESSAGE(rig.renderer.add_frame_pass(FramePassStage::AfterResolve, null_pass, &seen,
                                              "null pass", &rig.error),
                  rig.error);
  CHECK(rig.renderer.frame_pass_count() == 1);
  CapturedFrame with;
  REQUIRE_MESSAGE(shoot(rig, frame, with), rig.error);
  CHECK(seen.calls > 0);
  CHECK(seen.views == 1);
  CHECK(seen.width == k_width);
  CHECK(seen.height == k_height);
  CHECK(seen.view_width == static_cast<f32>(k_width));
  CHECK(seen.color);
  CHECK(seen.visibility);
  CHECK(seen.storage);
  CHECK(seen.dt_s == doctest::Approx(1.0 / 60.0));
  CHECK((seen.sky_params != 0) == sky);
  CHECK(rig.renderer.stats().visibility_m == 1234.0f);

  // The same picture through the linear image and the encode: the resolve's radiance rounded to a
  // half and encoded by the same function, so a code apart at most, where the half moved the value
  // across a rounding edge: measured 0.6% of the channels under the stand-in, 1.6% under the sky.
  REQUIRE(with.color.size() == before.color.size());
  u32 differ = 0;
  i32 worst = 0;
  for (u32 i = 0; i < before.color.size(); ++i) {
    const i32 d = std::abs(static_cast<i32>(with.color[i]) - static_cast<i32>(before.color[i]));
    if (d != 0) ++differ;
    if (d > worst) worst = d;
  }
  MESSAGE("null pass: " << differ << " of " << before.color.size() << " channels differ, worst "
                        << worst);
  CHECK(worst <= 1);
  CHECK(differ * 100 <= before.color.size() * 3);

  // A debug view is drawn as it was, without the pass.
  const u32 calls = seen.calls;
  CapturedFrame ids_view;
  REQUIRE_MESSAGE(
      shoot(rig, looking(time_s, static_cast<u32>(gfx::ResolveMode::ClusterColors)), ids_view),
      rig.error);
  CHECK(seen.calls == calls);

  // And with the pass removed, the frame is the bytes it was before the first registration.
  rig.renderer.remove_frame_pass(null_pass, &seen);
  CHECK(rig.renderer.frame_pass_count() == 0);
  CapturedFrame after;
  REQUIRE_MESSAGE(shoot(rig, frame, after), rig.error);
  CHECK(after.color == before.color);
  CHECK(seen.calls == calls);
}

}  // namespace

TEST_CASE("frame pass: a pass that draws nothing leaves the stand-in's picture, and none is it") {
  hold(false, 0.0);
}

#if ENGINE_FRAME_PASS_TESTS_SKY
TEST_CASE("frame pass: a pass that draws nothing leaves the sky's picture, and none is it") {
  REQUIRE(scene_gen::GeneratorRegistry::global().find_sky("earth") != nullptr);
  hold(true, 16.0 * 3600.0);
}
#endif

TEST_CASE("frame pass: the registration is refused past the limit and without a function") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, false), rig.error);
  std::string error;
  CHECK_FALSE(
      rig.renderer.add_frame_pass(FramePassStage::AfterResolve, nullptr, nullptr, "x", &error));
  CHECK_FALSE(error.empty());
  Seen seen[k_max_frame_passes + 1];
  for (u32 i = 0; i < k_max_frame_passes; ++i) {
    CHECK(rig.renderer.add_frame_pass(FramePassStage::AfterResolve, null_pass, &seen[i], "null",
                                      &error));
  }
  error.clear();
  CHECK_FALSE(rig.renderer.add_frame_pass(FramePassStage::AfterResolve, null_pass,
                                          &seen[k_max_frame_passes], "null", &error));
  CHECK_FALSE(error.empty());
  for (u32 i = 0; i < k_max_frame_passes; ++i)
    rig.renderer.remove_frame_pass(null_pass, &seen[i]);
  CHECK(rig.renderer.frame_pass_count() == 0);
}
