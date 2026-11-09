// The hand-off by distance (docs/subsystems/renderer.md, "The hand-off by distance"; roadmap G136).
// A sward patch near the camera is placed as its near form and its card, and the two hand over
// across a band of distance: the cull pass draws each only on its side of the band, and inside it
// the pool pass keeps a share of each form's parts. These cases hold the share's arithmetic (the
// CPU mirror the shader follows operation for operation) and the cull pass's band on every path:
// an instance outside its band draws nothing, inside it everything it drew with no role, and every
// rasterizer and the ray path agree to the id. Where the flora capability is built, the field kind's
// half too: a sward patch's near form keeps every blade short of its band, none past it and fewer
// as the band slides over it, and a patch with a role draws the same ids read whole and streamed.

#include "shadow_fixture.h"

#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#if ENGINE_RENDERER_TESTS_FLORA
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>
#endif

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <fstream>
#include <span>
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

// Every id word of two captures the same.
[[maybe_unused]] bool same_ids_of(const CapturedFrame& a, const CapturedFrame& b) {
  if (a.ids.size() != b.ids.size()) return false;
  for (u64 i = 0; i < a.ids.size(); ++i)
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

#if ENGINE_RENDERER_TESTS_FLORA

// ---- the field kind's thinning (the pool pass's half of the hand-off) ---------------------------

namespace {

// A sward patch as a field mesh, the recipe engine-view's field test cooks (authored_view_tests.cpp).
constexpr const char* k_sward =
    R"({"species":"sward","sward":{"species":"perennial_ryegrass","blades_m2":192,"density":0.5,)"
    R"("tillers_per_clump":4,"leaves_per_tiller":3,"clumpiness":0.6,"seed":8801,)"
    R"("triangle_budget":2400,"card_px":32}})";

constexpr u32 k_field_width = 256;
constexpr u32 k_field_height = 192;
// The eye stands 6 m from the patch's root, at a little over knee height, looking down at it.
const Vec3 k_field_eye{0.0f, 1.2f, 6.0f};
const Vec3 k_field_target{0.0f, 0.15f, 0.0f};
constexpr f32 k_field_d = 6.0f;  // the patch's distance from the eye, near enough

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A tunable set for the length of a scope and put back after it (rt_capacity_tests.cpp's).
struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

// The patch at the origin, and an anchor patch of no role well behind the eye, so the prefix is
// never empty and the patch is the second instance in both reads.
SceneInstance patch(u8 role) {
  SceneInstance p;
  p.mesh = 0;
  p.handoff = role;
  return p;
}
SceneInstance anchor() {
  SceneInstance a;
  a.mesh = 0;
  a.transform.position = Vec3{0.0f, 0.0f, 60.0f};
  return a;
}

struct FieldShot {
  CapturedFrame frame;
  u32 patch_pixels = 0;  // the patch's (instance 1's)
};

// The field drawn on the hardware rasterizer: the patch read whole (`streamed` false: in the
// prefix, `role` and `band` set on its record after the load when `band` is not 0, else the
// tunable's) or streamed (in a tail of its own, its band the tunable's, its stamp the first
// fill's).
bool draw_field(const gfx::Device& device, const test::TempDir& tmp, bool streamed, u8 role,
                u32 band, FieldShot& out, std::string& error) {
  SceneDesc desc;
  desc.meshes.push_back(tmp.file("sward.flora.json"));
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  desc.instances.push_back(anchor());
  if (!streamed) desc.instances.push_back(patch(role));
  SceneData data;
  if (!load_scene(desc, data, error)) return false;
  if (!streamed && band != 0) {
    const u32 bits = role == scene_gen::k_handoff_near  ? gfx::k_instance_handoff_near
                     : role == scene_gen::k_handoff_far ? gfx::k_instance_handoff_far
                                                        : 0u;
    gfx::set_instance_handoff(data.instances[1], bits, band, 0);
  }
  data.dynamic = streamed;
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;
  settings.shadows = ShadowMode::Off;
  settings.lights = false;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
    error = "the hardware rasterizer is unavailable here";
    return false;
  }
  if (!resolved.flora_motion) {
    error = "the field is not posed here";
    return false;
  }
  GpuScene scene;
  if (!scene.create(device, data, resolved, &error)) return false;
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_field_width;
  rd.height = k_field_height;
  if (!renderer.create(device, scene, resolved, rd, &error)) return false;
  if (streamed) {
    const SceneInstance tail = patch(role);
    if (!renderer.set_dynamic_instances(std::span<const SceneInstance>(&tail, 1), &error))
      return false;
  }
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), k_field_eye);
  frame.camera.target = absolute(WorldPos::origin(), k_field_target);
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  bool ok = renderer.capture(frame, channels, out.frame, &error) &&
            renderer.capture(frame, channels, out.frame, &error);
  if (ok) {
    for (u32 px = 0; px < k_field_width * k_field_height; ++px)
      out.patch_pixels += out.frame.ids[u64{px} * k_id_words] == 1u ? 1u : 0u;
  }
  renderer.destroy();
  scene.destroy();
  return ok;
}

}  // namespace

TEST_CASE("renderer: the field's near form keeps every blade short of its band, none past it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("renderer_handoff_field");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("sward.flora.json"), k_sward));
  std::string error;
  FieldShot plain;
  if (!draw_field(gpu.device, tmp, false, 0, 0, plain, error)) {
    MESSAGE("field not drawn here: " << error);
    return;
  }
  REQUIRE(plain.patch_pixels > 500);
  // The band slid over the patch, 4 m wide: past every root (every blade kept), centred 1 m past
  // the patch, on it, 1 m short of it (three quarters, a half, a quarter of the blades), and short
  // of every root (none). The share is a nested set — a blade kept at a smaller share is kept at
  // every larger one (its own hash against the share) — so the pixels can only fall.
  const f32 centres[] = {k_field_d + 7.0f, k_field_d + 1.0f, k_field_d, k_field_d - 1.0f,
                         k_field_d - 7.0f};
  u32 pixels[5] = {};
  for (u32 i = 0; i < 5; ++i) {
    FieldShot shot;
    const u32 band = gfx::pack_handoff_band(centres[i] - 2.0f, centres[i] + 2.0f);
    REQUIRE_MESSAGE(
        draw_field(gpu.device, tmp, false, scene_gen::k_handoff_near, band, shot, error), error);
    pixels[i] = shot.patch_pixels;
    if (i == 0) {
      // Short of the band the near form is exactly what it draws with no role.
      CHECK(same_ids_of(shot.frame, plain.frame));
    }
  }
  MESSAGE("the near form's pixels with the band past it, centred +1 m, on it, -1 m, short of it: "
          << pixels[0] << ", " << pixels[1] << ", " << pixels[2] << ", " << pixels[3] << ", "
          << pixels[4] << " (no role " << plain.patch_pixels << ")");
  CHECK(pixels[0] == plain.patch_pixels);
  CHECK(pixels[1] < pixels[0]);  // the pool pass thins it inside the band...
  CHECK(pixels[2] < pixels[1]);
  CHECK(pixels[3] < pixels[2]);
  CHECK(pixels[3] > 0);  // ...without dropping it whole
  CHECK(pixels[4] == 0);
}

TEST_CASE("renderer: a patch with a role draws the same ids read whole and streamed") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("renderer_handoff_field_streamed");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("sward.flora.json"), k_sward));
  // The tunable's band across the patch, so the share is neither all nor none: both reads place
  // the record with it, the streamed one as its tail's first fill (stamp 0, never faded).
  const ScopedTunable near_m("renderer.flora.sward_near_m", "6");
  const ScopedTunable fade_m("renderer.flora.sward_fade_m", "4");
  std::string error;
  FieldShot plain;
  if (!draw_field(gpu.device, tmp, false, 0, 0, plain, error)) {
    MESSAGE("field not drawn here: " << error);
    return;
  }
  for (const u8 role : {scene_gen::k_handoff_near, scene_gen::k_handoff_far}) {
    CAPTURE(static_cast<u32>(role));
    FieldShot whole;
    REQUIRE_MESSAGE(draw_field(gpu.device, tmp, false, role, 0, whole, error), error);
    FieldShot streamed;
    REQUIRE_MESSAGE(draw_field(gpu.device, tmp, true, role, 0, streamed, error), error);
    MESSAGE("role " << static_cast<u32>(role) << ": whole " << whole.patch_pixels
                    << " px, streamed " << streamed.patch_pixels << " px, no role "
                    << plain.patch_pixels << " px");
    CHECK(whole.patch_pixels > 0);
    CHECK(whole.patch_pixels < plain.patch_pixels);  // the band thins it in both reads
    CHECK(same_ids_of(whole.frame, streamed.frame));
    CHECK(std::memcmp(whole.frame.depth.data(), streamed.frame.depth.data(),
                      whole.frame.depth.size() * sizeof(whole.frame.depth[0])) == 0);
  }
}

#endif  // ENGINE_RENDERER_TESTS_FLORA
