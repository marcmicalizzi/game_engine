// The hand-off by distance (docs/subsystems/renderer.md, "The hand-off by distance"; roadmap G136).
// A sward patch near the camera is placed as its near form and its card, and the two hand over
// across a band of distance: the cull pass draws each only on its side of the band, and inside it
// the pool pass keeps a share of each form's parts. These cases hold the share's arithmetic (the
// CPU mirror the shader follows operation for operation) and the cull pass's band on every path:
// an instance outside its band draws nothing, inside it everything it drew with no role, and every
// rasterizer and the ray path agree to the id.

#include "shadow_fixture.h"

#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::renderer;
using namespace engine::renderer::shadow_fixture;

namespace {

constexpr u32 k_cube = 1;  // the fixture's cube: the instance given a role

struct Ids {
  Vector<u32> ids;
  bool available = true;
  std::string why;
};

// The fixture's cube drawn by `raster` with the role, band and stamp given; the ids, one word a
// pixel (the instance).
Ids draw(const gfx::Device& device, RasterMode raster, u32 role, u32 band) {
  Ids out;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(make_scene(Caster::cube, data, error), error);
  gfx::set_instance_handoff(data.instances[k_cube], role, band, 0);
  RenderSettings settings;
  settings.raster = raster;
  settings.shadows = ShadowMode::Off;
  settings.lights = false;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    out.available = false;
    out.why = unavailable_reason(availability, device);
    return out;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc desc;
  desc.width = k_width;
  desc.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, desc, &error), error);
  const Mat4 rotation = fixture_frame();
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), to_world(rotation, k_eye_local));
  frame.camera.target = absolute(WorldPos::origin(), to_world(rotation, k_target_local));
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame captured;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, captured, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, captured, &error), error);
  for (u32 px = 0; px < k_width * k_height; ++px)
    out.ids.push_back(captured.ids[px * k_id_words]);
  renderer.destroy();
  scene.destroy();
  return out;
}

bool same_ids(const Ids& a, const Ids& b) {
  if (a.ids.size() != b.ids.size()) return false;
  for (u32 i = 0; i < a.ids.size(); ++i)
    if (a.ids[i] != b.ids[i]) return false;
  return true;
}

u32 cube_pixels(const Ids& p) {
  u32 n = 0;
  for (const u32 id : p.ids)
    n += id == k_cube ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("renderer: the hand-off's share is whole short of the band, none past it, monotone") {
  const u32 band = gfx::pack_handoff_band(21.0f, 29.0f);
  CHECK(band == (2100u | (2900u << 16)));
  const u32 settled = gfx::k_handoff_ramp_frames;
  f32 last_near = 2.0f, last_far = -1.0f;
  for (f32 d = 15.0f; d <= 35.0f; d += 0.25f) {
    const f32 near = gfx::handoff_keep(gfx::k_instance_handoff_near, band, d, settled);
    const f32 far = gfx::handoff_keep(gfx::k_instance_handoff_far, band, d, settled);
    CAPTURE(d);
    CHECK(near <= last_near);  // the near form thins out with distance
    CHECK(far >= last_far);    // and the card thickens in
    CHECK(near + far == doctest::Approx(1.0f).epsilon(1e-5));  // between them, the whole patch
    if (d <= 21.0f) CHECK(near == 1.0f);
    if (d >= 29.0f) CHECK(near == 0.0f);
    if (d <= 21.0f) CHECK(far == 0.0f);
    if (d >= 29.0f) CHECK(far == 1.0f);
    last_near = near;
    last_far = far;
  }
  // The load-in: a near form grows from nothing over the ramp, and while it does its card keeps
  // every part at any distance, so the pair never shows less than the card did.
  for (u32 age = 0; age <= settled; ++age) {
    const f32 grown = static_cast<f32>(age) / static_cast<f32>(settled);
    CHECK(gfx::handoff_keep(gfx::k_instance_handoff_near, band, 10.0f, age) ==
          doctest::Approx(grown));
    CHECK(gfx::handoff_keep(gfx::k_instance_handoff_far, band, 10.0f, age) ==
          doctest::Approx(1.0f - grown));
  }
  // A stamp is counted modulo 2^22, and 0 is settled.
  CHECK(gfx::handoff_age(0, 12345) == settled);
  CHECK(gfx::handoff_age(5, 9) == 4);
  CHECK(gfx::handoff_age(gfx::k_instance_stamp_mask, 3) == 4);
  // No role: everything, and a record with no role carries no band.
  gfx::InstanceDesc plain;
  gfx::set_instance_handoff(plain, 0, band, 7);
  CHECK(plain.band == 0);
  CHECK((plain.flags & gfx::k_instance_handoff_mask) == 0);
  CHECK(gfx::handoff_keep(0, band, 1000.0f, 0) == 1.0f);
  gfx::InstanceDesc near;
  near.flags = gfx::k_instance_uniform_scale | gfx::k_instance_untraced;
  gfx::set_instance_handoff(near, gfx::k_instance_handoff_near, band, 77);
  CHECK((near.flags & gfx::k_instance_untraced) != 0);  // the caster policy's bits stay
  CHECK(((near.flags >> gfx::k_instance_stamp_shift) & gfx::k_instance_stamp_mask) == 77);
}

TEST_CASE("renderer: an instance outside its hand-off band draws nothing, inside it everything") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  // The cube is about 6.8 m from the eye; a band 100 m out holds it in the near side, one 2 m
  // out holds it past the near side's end and short of the far side's start.
  const u32 inside_near = gfx::pack_handoff_band(90.0f, 100.0f);
  const u32 outside_near = gfx::pack_handoff_band(1.0f, 2.0f);
  const u32 outside_far = gfx::pack_handoff_band(90.0f, 100.0f);
  const u32 inside_far = gfx::pack_handoff_band(1.0f, 2.0f);
  const Ids plain = draw(gpu.device, RasterMode::Hardware, 0, 0);
  REQUIRE(plain.available);
  const u32 drawn = cube_pixels(plain);
  REQUIRE(drawn > 500);
  for (const RasterMode raster :
       {RasterMode::Hardware, RasterMode::Vertex, RasterMode::Software, RasterMode::RayTrace}) {
    CAPTURE(static_cast<u32>(raster));
    const Ids none = draw(gpu.device, raster, 0, 0);
    if (!none.available) {
      MESSAGE("path " << static_cast<u32>(raster) << " unavailable here: " << none.why);
      continue;
    }
    const Ids near_in = draw(gpu.device, raster, gfx::k_instance_handoff_near, inside_near);
    const Ids near_out = draw(gpu.device, raster, gfx::k_instance_handoff_near, outside_near);
    const Ids far_in = draw(gpu.device, raster, gfx::k_instance_handoff_far, inside_far);
    const Ids far_out = draw(gpu.device, raster, gfx::k_instance_handoff_far, outside_far);
    MESSAGE("path " << static_cast<u32>(raster) << ": cube " << cube_pixels(none) << " px; near "
                    << cube_pixels(near_in) << " in, " << cube_pixels(near_out) << " out; far "
                    << cube_pixels(far_in) << " in, " << cube_pixels(far_out) << " out");
    // Inside the band: the bytes it drew with no role. Outside: nothing.
    CHECK(same_ids(near_in, none));
    CHECK(same_ids(far_in, none));
    CHECK(cube_pixels(near_out) == 0);
    CHECK(cube_pixels(far_out) == 0);
    // Every path draws what the hardware rasterizer draws.
    CHECK(same_ids(none, plain));
  }
}
