// Sampling over time's inputs (docs/subsystems/renderer.md, "Sampling over time: the inputs";
// ADR-0064; roadmap R154): the sub-pixel offset, and the motion pass's vectors against the CPU.
//
// **What a motion vector is held to.** A pixel's word is the pixels its surface point moved across
// the picture since the last frame, with neither frame's offset in it. The CPU's side of that is
// the pixel's own point — its NDC as drawn and the depth the visibility buffer holds, taken through
// the inverse of the frame's offset view-projection in double — moved to where it was (the eyes'
// difference for a still surface, the instance's two transforms for a moved one) and projected
// through last frame's un-offset view-projection. The two agree within 0.02 px: the half float's
// step is 1/128 px under 16 px of motion, so its rounding is 0.004, and the rest is the shader's
// float arithmetic against the double here.
//
// A swaying tree and a walking skinned actor are held against CPU mirrors of their deformers
// (`gfx::flora_bend` over the poses `flora_pose_instances` gives, `anim::skin_positions`), over
// three frames with two in flight, so "last frame's inputs" is read from the other frame slot's
// part poses and bone matrices both ways round.
#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/anim/skeleton.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/device.h>
#include <domain/gfx/flora_motion.h>
#include <domain/gfx/motion.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/flora_poser.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(renderer_test::device_options(), &error)) {
      why = "no Vulkan device: " + error;
      if (!renderer_test::device_profile().empty()) {
        FAIL_CHECK("ENGINE_GFX_TEST_DEVICE=" << renderer_test::device_profile() << ": " << why);
      }
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
  ~Gpu() { device.destroy(); }
};

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

constexpr u32 k_width = 256;
constexpr u32 k_height = 144;

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings) {
    if (!load_scene(desc, data, error)) return false;
    return finish(device, settings);
  }

  // The same from a `SceneData` the caller filled in itself (the skinned bar), with two frames in
  // flight: the motion pass's "last frame" is the other slot's inputs.
  bool finish(const gfx::Device& device, const RenderSettings& settings) {
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.frames_in_flight = 2;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

// The picture's own rasterizer on whatever the device is (mesh shaders, or the vertex tier), no
// shadows: one frame a capture, so the views the renderer holds after it are the captured frame's.
RenderSettings plain_settings() {
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;
  settings.shadows = ShadowMode::Off;
  return settings;
}

// 4x4 in double, row then column: the reference's own arithmetic, not the renderer's.
struct M4 {
  f64 m[4][4] = {};
};

M4 widen(const Mat4& a) {
  M4 out;
  for (u32 r = 0; r < 4; ++r)
    for (u32 c = 0; c < 4; ++c)
      out.m[r][c] = static_cast<f64>(a.at(r, c));
  return out;
}

void apply(const M4& a, const f64 v[4], f64 out[4]) {
  for (u32 r = 0; r < 4; ++r)
    out[r] = a.m[r][0] * v[0] + a.m[r][1] * v[1] + a.m[r][2] * v[2] + a.m[r][3] * v[3];
}

M4 inverse_of(const M4& a) {
  f64 w[4][8];
  for (u32 r = 0; r < 4; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      w[r][c] = a.m[r][c];
      w[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (u32 col = 0; col < 4; ++col) {
    u32 pivot = col;
    for (u32 r = col + 1; r < 4; ++r) {
      if (std::fabs(w[r][col]) > std::fabs(w[pivot][col])) pivot = r;
    }
    REQUIRE(w[pivot][col] != 0.0);
    for (u32 c = 0; c < 8; ++c) {
      const f64 t = w[col][c];
      w[col][c] = w[pivot][c];
      w[pivot][c] = t;
    }
    const f64 inv = 1.0 / w[col][col];
    for (u32 c = 0; c < 8; ++c)
      w[col][c] *= inv;
    for (u32 r = 0; r < 4; ++r) {
      if (r == col) continue;
      const f64 f = w[r][col];
      for (u32 c = 0; c < 8; ++c)
        w[r][c] -= f * w[col][c];
    }
  }
  M4 out;
  for (u32 r = 0; r < 4; ++r)
    for (u32 c = 0; c < 4; ++c)
      out.m[r][c] = w[r][c + 4];
  return out;
}

// One captured frame and the view it was drawn with.
struct Shot {
  CapturedFrame frame;
  View view;
  WorldPos eye;
};

bool shoot(Rig& rig, const FrameDesc& frame, Shot& out, std::string& why) {
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  channels.motion = true;
  if (!rig.renderer.capture(frame, channels, out.frame, &why)) return false;
  out.view = rig.renderer.views()[0];
  out.eye = frame.camera.position;
  return true;
}

// Where the point under pixel (x, y) of `now` is in `now`'s space (its eye at the origin), from the
// depth the frame captured: homogeneous, so an empty pixel (depth 0) is a direction.
void point_of(const Shot& now, const M4& inverse, u32 x, u32 y, f64 out[4]) {
  const f64 ndc_x = (static_cast<f64>(x) + 0.5) / k_width * 2.0 - 1.0;
  const f64 ndc_y = 1.0 - (static_cast<f64>(y) + 0.5) / k_height * 2.0;
  const f64 clip[4] = {ndc_x, ndc_y, static_cast<f64>(now.frame.depth[y * k_width + x]), 1.0};
  apply(inverse, clip, out);
}

// The motion the pass should have written for pixel (x, y) of `now`, whose point was at `was`
// (homogeneous, in `before`'s space) in the frame before. False when it was behind that camera.
bool expected_motion(const Shot& now, const Shot& before, const f64 was[4], u32 x, u32 y, f64& mx,
                     f64& my) {
  f64 clip[4];
  apply(widen(before.view.view_proj_center), was, clip);
  if (!(clip[3] > 0.0)) return false;
  const f64 ndc_x = (static_cast<f64>(x) + 0.5) / k_width * 2.0 - 1.0;
  const f64 ndc_y = 1.0 - (static_cast<f64>(y) + 0.5) / k_height * 2.0;
  // The pixel's point without this frame's offset: the sample sits `offset` pixels from the centre.
  const f64 here_x = ndc_x + 2.0 * static_cast<f64>(now.view.offset_x) / k_width;
  const f64 here_y = ndc_y - 2.0 * static_cast<f64>(now.view.offset_y) / k_height;
  mx = (here_x - clip[0] / clip[3]) * 0.5 * k_width;
  my = (here_y - clip[1] / clip[3]) * -0.5 * k_height;
  return true;
}

// The worst difference, in pixels, between the pass's vectors and the reference's over every pixel
// `take` accepts, with `was` giving where each pixel's point was.
template <typename Take, typename Was>
f64 worst_error(const Shot& now, const Shot& before, Take take, Was was, u32& counted,
                f64& longest) {
  const M4 inverse = inverse_of(widen(now.view.view_proj));
  f64 worst = 0.0;
  counted = 0;
  longest = 0.0;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 p = y * k_width + x;
      if (!take(p)) continue;
      f64 here[4];
      point_of(now, inverse, x, y, here);
      f64 then[4];
      was(here, then);
      f64 mx = 0.0;
      f64 my = 0.0;
      if (!expected_motion(now, before, then, x, y, mx, my)) continue;
      const f64 ex = static_cast<f64>(now.frame.motion[p * 2]) - mx;
      const f64 ey = static_cast<f64>(now.frame.motion[p * 2 + 1]) - my;
      worst = std::max(worst, std::sqrt(ex * ex + ey * ey));
      longest = std::max(longest, std::sqrt(mx * mx + my * my));
      ++counted;
    }
  }
  return worst;
}

// A still surface's point, moved from this frame's eye to last frame's.
auto still(const Shot& now, const Shot& before) {
  const DVec3 d = now.eye - before.eye;
  return [d](const f64 here[4], f64 then[4]) {
    then[0] = here[0] + d.x * here[3];
    then[1] = here[1] + d.y * here[3];
    then[2] = here[2] + d.z * here[3];
    then[3] = here[3];
  };
}

constexpr f64 k_tolerance_px = 0.02;

}  // namespace

// The offset is a shear: every point lands the offset's pixels the other way, the LOD's scale does
// not move, and no offset is the matrices' own bits. No device.
TEST_CASE("motion: the sub-pixel offset moves a view's picture by exactly its pixels") {
  ViewSet set;
  std::string error;
  REQUIRE_MESSAGE(set.build(ViewSetDesc{}, 1920, 1080, &error), error);
  Camera camera;
  camera.position = WorldPos::origin() + DVec3{3.0, 2.0, 9.0};
  camera.target = WorldPos::origin() + DVec3{0.0, 1.0, 0.0};
  set.update(camera);
  const View plain = set[0];
  CHECK(std::memcmp(&plain.view_proj, &plain.view_proj_center, sizeof(Mat4)) == 0);
  CHECK(plain.offset_x == 0.0f);
  CHECK(plain.offset_y == 0.0f);

  const f32 jx = 0.25f;
  const f32 jy = -0.375f;
  set.update(camera, jx, jy);
  const View moved = set[0];
  CHECK(std::memcmp(&moved.view_proj_center, &plain.view_proj, sizeof(Mat4)) == 0);
  CHECK(moved.proj_scale == plain.proj_scale);
  CHECK(moved.offset_x == jx);
  CHECK(moved.offset_y == jy);
  // Points in the frame's space (the eye at the origin), in front of the camera.
  const Vec3 forward = camera_forward(camera);
  for (const Vec3 side :
       {Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.5f, 0.4f, 0.0f}, Vec3{-2.0f, -1.0f, 0.5f}}) {
    const Vec3 point = forward * 7.0f + side;
    const Vec4 a = plain.view_proj * Vec4{point.x, point.y, point.z, 1.0f};
    const Vec4 b = moved.view_proj * Vec4{point.x, point.y, point.z, 1.0f};
    const f32 dx = (b.x / b.w - a.x / a.w) * 0.5f * 1920.0f;
    const f32 dy = (b.y / b.w - a.y / a.w) * -0.5f * 1080.0f;
    CHECK(dx == doctest::Approx(-jx).epsilon(1e-3));
    CHECK(dy == doctest::Approx(-jy).epsilon(1e-3));
    CHECK(b.z == a.z);  // the depth a pixel holds does not move
    CHECK(b.w == a.w);
  }
  // The sequence: eight offsets inside the pixel, all different.
  f32 xs[k_sample_phases];
  f32 ys[k_sample_phases];
  for (u32 k = 0; k < k_sample_phases; ++k) {
    sample_offset(k, xs[k], ys[k]);
    CHECK(xs[k] > -0.5f);
    CHECK(xs[k] < 0.5f);
    CHECK(ys[k] > -0.5f);
    CHECK(ys[k] < 0.5f);
    for (u32 j = 0; j < k; ++j)
      CHECK((xs[j] != xs[k] || ys[j] != ys[k]));
  }
  CHECK(xs[0] == 0.0f);  // Halton(2) at 1 is a half
  CHECK(xs[1] == -0.25f);
}

// Off: no buffer, no offset, no channel. What every other test of the renderer runs as.
TEST_CASE("motion: without the inputs the renderer keeps and draws nothing of them") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  SceneDesc desc;
  desc.meshes.push_back("");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, plain_settings()), rig.error);
  CHECK_FALSE(rig.resolved.reconstruction_inputs);
  CHECK(rig.renderer.motion_bytes() == 0);
  CHECK_FALSE(rig.renderer.reconstruction().inputs);
  CHECK(rig.renderer.reconstruction().native());
  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 22.0f, 0.4f, k_orbit_pitch);
  CaptureChannels channels;
  CapturedFrame plain;
  std::string why;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, plain, &why), why);
  const View view = rig.renderer.views()[0];
  CHECK(view.offset_x == 0.0f);
  CHECK(view.offset_y == 0.0f);
  CHECK(std::memcmp(&view.view_proj, &view.view_proj_center, sizeof(Mat4)) == 0);
  CHECK(rig.renderer.stats().last.gpu_motion == 0.0);
  channels.motion = true;
  CapturedFrame refused;
  CHECK_FALSE(rig.renderer.capture(frame, channels, refused, &why));
  CHECK(why.find("renderer.reconstruction.inputs") != std::string::npos);
}

// A camera turned and moved between two frames, over a still scene and its sky: every pixel's
// vector is where its point was, and a frame with no frame before it measures from itself. Fails
// without the pass's reprojection (the vectors would be zero) and with the offset left in a vector
// (the first frame's would be the offset's quarter pixel and more).
TEST_CASE("motion: a camera's turn and step reproject every pixel of a still scene") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable inputs("renderer.reconstruction.inputs", "true");
  SceneDesc desc;
  desc.meshes.push_back("");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, plain_settings()), rig.error);
  REQUIRE(rig.resolved.reconstruction_inputs);
  CHECK(rig.renderer.motion_bytes() >= u64{k_width} * k_height * 12);
  CHECK(rig.renderer.reconstruction().inputs);
  CHECK(rig.renderer.reconstruction().native());  // the inputs alone are still a native frame

  std::string why;
  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 22.0f, 0.4f, k_orbit_pitch);
  frame.history_reset = true;
  Shot first;
  REQUIRE_MESSAGE(shoot(rig, frame, first, why), why);
  u32 counted = 0;
  f64 longest = 0.0;
  const auto every = [](u32) { return true; };
  f64 worst = worst_error(first, first, every, still(first, first), counted, longest);
  MESSAGE("no frame before: worst " << worst << " px over " << counted << " pixels");
  CHECK(counted == k_width * k_height);
  CHECK(worst <= k_tolerance_px);
  // The reference itself: a point is where it is, to the two float matrices' rounding. The frame's
  // offset view-projection and its un-offset one are each a float product (entries rounded at
  // 2^-24 of their size, four products and three sums a row), so a point taken out through one
  // and back through the other is off by under 8 * 2^-24 in NDC, 6e-5 px at half this width.
  CHECK(longest <= 1e-4);

  // The next frame: the camera turned and stepped, drawn at the next phase's offset.
  frame.history_reset = false;
  const f64 step = static_cast<f64>(rig.data.radius) * 0.02;
  frame.camera.position += DVec3{step, step * 0.5, -step};
  frame.camera.target += DVec3{-step * 2.0, step, step};
  Shot second;
  REQUIRE_MESSAGE(shoot(rig, frame, second, why), why);
  CHECK((second.view.offset_x != 0.0f || second.view.offset_y != 0.0f));
  CHECK(
      (second.view.offset_x != first.view.offset_x || second.view.offset_y != first.view.offset_y));
  worst = worst_error(second, first, every, still(second, first), counted, longest);
  u32 covered = 0;
  for (u32 p = 0; p < k_width * k_height; ++p)
    covered += second.frame.depth[p] != 0.0f ? 1u : 0u;
  MESSAGE("turned and stepped: worst " << worst << " px over " << counted << " pixels, " << covered
                                       << " of them covered, the longest vector " << longest
                                       << " px");
  CHECK(counted == k_width * k_height);
  // The fixture's own premise, not a bound on the pass: both a surface and a sky in the picture
  // (the heightfield from this orbit covers about a fifth of it).
  CHECK(covered > k_width * k_height / 8);
  CHECK(covered < k_width * k_height);
  CHECK(longest > 1.0);
  CHECK(longest < 16.0);  // under 16 px a half float's step is 1/128 px (the tolerance's premise)
  CHECK(worst <= k_tolerance_px);
}

// A rigid instance moved and scaled between two frames under a still camera: its pixels' vectors
// are its two transforms', and everything else stands still. Fails without the moved table (the
// mover's vectors would be zero, a pixel and more short).
TEST_CASE("motion: a moved rigid instance's pixels follow its two transforms") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable inputs("renderer.reconstruction.inputs", "true");
  SceneDesc desc;
  desc.meshes.push_back("");
  SceneInstance ground;
  desc.instances.push_back(ground);
  SceneInstance mover;
  mover.transform.position = Vec3{0.0f, 2.0f, 0.0f};
  mover.transform.scale = Vec3{0.25f, 0.25f, 0.25f};
  desc.instances.push_back(mover);
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, plain_settings()), rig.error);

  std::string why;
  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 22.0f, 0.4f, k_orbit_pitch);
  frame.history_reset = true;
  Shot first;
  REQUIRE_MESSAGE(shoot(rig, frame, first, why), why);

  SceneInstance placed = mover;
  const f32 nudge = rig.data.radius * 0.02f;
  placed.transform.position = mover.transform.position + Vec3{nudge, nudge * 0.5f, -nudge};
  placed.transform.scale = Vec3{0.3f, 0.3f, 0.3f};
  const u32 index = 1;
  REQUIRE_MESSAGE(rig.renderer.move_instances(std::span<const u32>(&index, 1),
                                              std::span<const SceneInstance>(&placed, 1), &why),
                  why);
  frame.history_reset = false;
  Shot second;
  REQUIRE_MESSAGE(shoot(rig, frame, second, why), why);

  // The mover's point taken back through its two placements: both are a uniform scale about the
  // instance's own origin and a translation, so the point from the origin scales by their ratio.
  const DVec3 eye = frame.camera.position - WorldPos::origin();
  const DVec3 now_at = DVec3{placed.transform.position} - eye;
  const DVec3 was_at = DVec3{mover.transform.position} - eye;
  const f64 ratio =
      static_cast<f64>(mover.transform.scale.x) / static_cast<f64>(placed.transform.scale.x);
  const auto through = [=](const f64 here[4], f64 then[4]) {
    const f64 w = here[3];
    then[0] = (here[0] - now_at.x * w) * ratio + was_at.x * w;
    then[1] = (here[1] - now_at.y * w) * ratio + was_at.y * w;
    then[2] = (here[2] - now_at.z * w) * ratio + was_at.z * w;
    then[3] = w;
  };
  const auto of_instance = [&](u32 instance) {
    return [&second, instance](u32 p) { return second.frame.ids[p * k_id_words] == instance; };
  };
  u32 counted = 0;
  f64 longest = 0.0;
  f64 worst = worst_error(second, first, of_instance(1), through, counted, longest);
  MESSAGE("the mover: worst " << worst << " px over " << counted << " pixels, the longest vector "
                              << longest << " px");
  CHECK(counted > 100);
  CHECK(longest > 1.0);
  CHECK(longest < 16.0);
  CHECK(worst <= k_tolerance_px);
  worst = worst_error(second, first, of_instance(0), still(second, first), counted, longest);
  MESSAGE("the ground under it: worst " << worst << " px over " << counted << " pixels");
  CHECK(counted > 100);
  CHECK(worst <= k_tolerance_px);

  // A frame later it has not moved again: its entry is now where it stands, and its vectors are
  // the still scene's.
  Shot third;
  REQUIRE_MESSAGE(shoot(rig, frame, third, why), why);
  worst =
      worst_error(third, second, [](u32) { return true; }, still(third, second), counted, longest);
  MESSAGE("a frame after the move: worst " << worst << " px");
  CHECK(worst <= k_tolerance_px);
}

// A deformed surface (the procedural deformer's wave, which every instance of the scene takes): its
// pixels go through the two pools' corners. With the deformer's clock held and the camera moved the
// vectors are the still scene's, which holds the corners, the barycentrics and the transform to the
// reference; with the camera held and the clock moved the surface's own motion appears, and with
// both held it is gone again, which is the second pool holding last frame's shape and not this
// frame's. Fails without the second write (the clock's step would leave every vector zero).
TEST_CASE("motion: a deformed surface's pixels are measured from last frame's shape") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable inputs("renderer.reconstruction.inputs", "true");
  SceneDesc desc;
  desc.meshes.push_back("");
  RenderSettings settings = plain_settings();
  settings.deform = true;
  settings.deform_kind = gfx::k_deform_wave;
  settings.deform_amplitude = 0.04f;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings), rig.error);
  REQUIRE(rig.resolved.deform_pass);
  CHECK(rig.renderer.motion_bytes() > rig.scene.deform_pool_bytes());  // the second pool is in it

  std::string why;
  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 22.0f, 0.4f, k_orbit_pitch);
  frame.frame_index = 7;
  frame.history_reset = true;
  Shot first;
  REQUIRE_MESSAGE(shoot(rig, frame, first, why), why);

  // The clock held, the camera moved: the still scene's vectors, through the deformed path.
  frame.history_reset = false;
  const f64 step = static_cast<f64>(rig.data.radius) * 0.02;
  frame.camera.position += DVec3{step, step * 0.5, -step};
  Shot second;
  REQUIRE_MESSAGE(shoot(rig, frame, second, why), why);
  const auto covered = [](const Shot& shot) {
    return [&shot](u32 p) { return shot.frame.depth[p] != 0.0f; };
  };
  u32 counted = 0;
  f64 longest = 0.0;
  f64 worst = worst_error(second, first, covered(second), still(second, first), counted, longest);
  MESSAGE("the clock held, the camera moved: worst "
          << worst << " px over " << counted << " pixels, the longest vector " << longest << " px");
  CHECK(counted > k_width * k_height / 8);  // the fixture's premise: a fifth of the picture
  CHECK(longest > 1.0);
  CHECK(worst <= k_tolerance_px);

  // The camera held, the clock moved a third of a second: the surface's own motion.
  frame.frame_index = 27;
  Shot third;
  REQUIRE_MESSAGE(shoot(rig, frame, third, why), why);
  u32 moving = 0;
  f64 sum = 0.0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (third.frame.depth[p] == 0.0f) continue;
    const f64 length = std::hypot(static_cast<f64>(third.frame.motion[p * 2]),
                                  static_cast<f64>(third.frame.motion[p * 2 + 1]));
    sum += length;
    moving += length > 0.05 ? 1u : 0u;
  }
  MESSAGE("the camera held, the clock moved: " << moving << " of " << counted
                                               << " covered pixels moved more than 0.05 px, mean "
                                               << sum / counted << " px");
  CHECK(moving * 4 > counted);

  // Both held: last frame's shape is this frame's, and nothing moves.
  Shot fourth;
  REQUIRE_MESSAGE(shoot(rig, frame, fourth, why), why);
  worst = worst_error(fourth, third, covered(fourth), still(fourth, third), counted, longest);
  MESSAGE("both held: worst " << worst << " px");
  CHECK(worst <= k_tolerance_px);
}

// ---- deformed instances against their deformers' CPU mirrors ----------------------------------
//
// The reference for a deformed pixel is built without the pools: the triangle the ids name, its
// corners deformed on the CPU by the deformer's own mirror for this frame's inputs and for last
// frame's, the pixel's ray met with the first triangle in double, and the point carried to the
// second by its barycentrics. The pass reads the same corners out of its two pools, so the two
// agree to the pools' float rounding and the half float's step, and a "last frame" that is really
// this frame's (the other slot's region rewritten, or never read) leaves the deformer's whole
// motion as the error.
namespace {

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
D3 operator+(D3 a, D3 b) { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, f64 s) { return D3{a.x * s, a.y * s, a.z * s}; }
f64 dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) {
  return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// The mesh's rest pose as the pool reads it: off its 16-bit grid.
Vector<Vec3> grid_positions(const geometry::ClusterMesh& mesh) {
  Vector<Vec3> out(mesh.vertices.size());
  for (u32 v = 0; v < out.size(); ++v)
    out[v] = geometry::dequantize_position(mesh, v);
  return out;
}

// Mesh-space positions placed by `instance` in the space of a frame whose eye is `eye` (the eye at
// the origin), through the matrix the renderer itself builds for that eye.
Vector<D3> placed(std::span<const Vec3> positions, const gfx::InstanceDesc& instance,
                  const WorldPos& eye) {
  Camera camera;
  camera.position = eye;
  const M4 to_frame = widen(gfx::instance_matrix(instance, frame_eye(camera)));
  const u32 count = static_cast<u32>(positions.size());
  Vector<D3> out(count);
  for (u32 v = 0; v < count; ++v) {
    const f64 p[4] = {static_cast<f64>(positions[v].x), static_cast<f64>(positions[v].y),
                      static_cast<f64>(positions[v].z), 1.0};
    f64 q[4];
    apply(to_frame, p, q);
    out[v] = D3{q[0], q[1], q[2]};
  }
  return out;
}

// The worst difference, in pixels, between the pass's vectors and the mirror's over the pixels of
// `instance`. `now_corners` are the mesh's vertices as this frame deformed and placed them, in its
// space; `then_corners` as last frame's inputs did, in last frame's.
f64 worst_deformed(const Shot& now, const Shot& before, u32 instance,
                   const geometry::ClusterMesh& mesh, const Vector<D3>& now_corners,
                   const Vector<D3>& then_corners, u32& counted, f64& longest, f64& worst_sine) {
  worst_sine = 1.0;
  const M4 inverse = inverse_of(widen(now.view.view_proj));
  f64 worst = 0.0;
  counted = 0;
  longest = 0.0;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 p = y * k_width + x;
      const u32* id = &now.frame.ids[u64{p} * k_id_words];
      if (id[0] != instance || id[1] >= mesh.clusters.size()) continue;
      const geometry::ClusterDesc& cluster = mesh.clusters[id[1]];
      const u32 packed = mesh.triangles[cluster.triangle_offset + id[2]];
      D3 a[3];
      D3 b[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k);
        a[k] = now_corners[v];
        b[k] = then_corners[v];
      }
      // The pixel's ray (the eye is the origin) met with the triangle's plane.
      f64 ray[4];
      point_of(now, inverse, x, y, ray);
      if (ray[3] == 0.0) continue;
      const D3 dir{ray[0] / ray[3], ray[1] / ray[3], ray[2] / ray[3]};
      const D3 e1 = a[1] - a[0];
      const D3 e2 = a[2] - a[0];
      const D3 normal = cross3(e1, e2);
      const f64 facing = dot3(normal, dir);
      if (facing == 0.0) continue;
      const D3 d = dir * (dot3(normal, a[0]) / facing) - a[0];
      const f64 d11 = dot3(e1, e1);
      const f64 d12 = dot3(e1, e2);
      const f64 d22 = dot3(e2, e2);
      const f64 denom = d11 * d22 - d12 * d12;
      if (!(denom > 0.0)) continue;
      const f64 v = (d22 * dot3(d, e1) - d12 * dot3(d, e2)) / denom;
      const f64 w = (d11 * dot3(d, e2) - d12 * dot3(d, e1)) / denom;
      const D3 then = b[0] * (1.0 - v - w) + b[1] * v + b[2] * w;
      const f64 was[4] = {then.x, then.y, then.z, 1.0};
      f64 mx = 0.0;
      f64 my = 0.0;
      if (!expected_motion(now, before, was, x, y, mx, my)) continue;
      const f64 ex = static_cast<f64>(now.frame.motion[p * 2]) - mx;
      const f64 ey = static_cast<f64>(now.frame.motion[p * 2 + 1]) - my;
      const f64 error = std::sqrt(ex * ex + ey * ey);
      if (error > worst) {
        worst = error;
        // Where it was: the sine of the triangle's angle at its first corner says how much of a
        // sliver it is, which is what float barycentrics lose bits to.
        worst_sine = std::sqrt(dot3(normal, normal) / (d11 * d22));
      }
      longest = std::max(longest, std::sqrt(mx * mx + my * my));
      ++counted;
    }
  }
  return worst;
}

// A two-bone bar (the skinning fixture of renderer_tests.cpp, smaller): a square column from y = 0
// to y = 2, its rings bound root to tip by height, so a turn of the tip joint bends the top half.
struct SkinnedBar {
  anim::Skeleton skeleton;
  Vector<Vec3> positions;
  Vector<geometry::SkinBinding> bindings;
  Vector<u32> indices;
  static constexpr u32 k_rings = 17;
  static constexpr f32 k_height = 2.0f;
  static constexpr f32 k_half = 0.25f;
};

void build_skinned_bar(SkinnedBar& bar) {
  bar.skeleton.resize(2);
  bar.skeleton.names[0] = "root";
  bar.skeleton.names[1] = "tip";
  bar.skeleton.parents[0] = anim::k_no_joint;
  bar.skeleton.parents[1] = 0;
  bar.skeleton.local_bind[0] = Transform3::identity();
  bar.skeleton.local_bind[1] = Transform3::identity();
  bar.skeleton.local_bind[1].position = Vec3{0.0f, 0.5f * SkinnedBar::k_height, 0.0f};
  anim::compute_inverse_bind(bar.skeleton);
  const f32 h = SkinnedBar::k_half;
  const Vec3 ring[4] = {Vec3{-h, 0, -h}, Vec3{h, 0, -h}, Vec3{h, 0, h}, Vec3{-h, 0, h}};
  for (u32 r = 0; r < SkinnedBar::k_rings; ++r) {
    const f32 t = static_cast<f32>(r) / static_cast<f32>(SkinnedBar::k_rings - 1);
    const f32 tip = t < 0.25f ? 0.0f : (t > 0.75f ? 1.0f : (t - 0.25f) * 2.0f);
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {1.0f - tip, tip, 0.0f, 0.0f};
    const geometry::SkinBinding binding = geometry::make_skin_binding(joints, weights);
    for (const Vec3& c : ring) {
      bar.positions.push_back(Vec3{c.x, t * SkinnedBar::k_height, c.z});
      bar.bindings.push_back(binding);
    }
  }
  for (u32 r = 0; r + 1 < SkinnedBar::k_rings; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      const u32 a = r * 4 + c;
      const u32 b = r * 4 + (c + 1) % 4;
      const u32 tri[6] = {a, a + 4, b, b, a + 4, b + 4};  // counter-clockwise seen from outside
      for (const u32 i : tri)
        bar.indices.push_back(i);
    }
  }
  const u32 top = (SkinnedBar::k_rings - 1) * 4;
  const u32 caps[12] = {0, 2, 1, 0, 3, 2, top, top + 1, top + 2, top, top + 2, top + 3};
  for (const u32 i : caps)
    bar.indices.push_back(i);
}

// The bar as a one-instance `SceneData`, skinned by its two joints, its spheres padded by
// `padding` metres for the bend.
bool make_bar_scene(const SkinnedBar& bar, f32 padding, SceneData& out, std::string& error) {
  geometry::AttributeSource attributes;
  attributes.skin =
      std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size());
  attributes.joint_count = bar.skeleton.joint_count();
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(bar.positions.data(), bar.positions.size()),
          std::span<const u32>(bar.indices.data(), bar.indices.size()),
          geometry::ClusterLodOptions{}, out.lod, &error, attributes)) {
    return false;
  }
  geometry::ClusterMeshPart part;
  part.cluster_count = out.lod.mesh.clusters.size();
  part.leaf_cluster_count = out.lod.level_cluster_counts[0];
  part.quant_origin = out.lod.mesh.quant_origin;
  part.quant_scale = out.lod.mesh.quant_scale;
  out.parts.push_back(part);
  SourceMesh source;
  source.part_material.push_back(-1);
  source.part_of_cluster.resize(part.cluster_count, 0u);
  out.sources.push_back(std::move(source));
  out.mesh_fit.push_back(Mat4::identity());  // what `move_instances` places the mesh by
  gfx::InstanceDesc instance{};
  gfx::set_instance_transform(instance, Mat4::identity());
  instance.first_pair = 0;
  instance.bounds_padding = padding;
  out.pair_count = part.cluster_count;
  out.instances.push_back(instance);
  out.instance_joints.push_back(static_cast<u32>(bar.skeleton.joint_count()));
  out.skinned_instances = 1;
  out.max_joints = static_cast<u32>(bar.skeleton.joint_count());
  update_scene_bounds(out);
  return true;
}

// The bar's skinning matrices for a bend of the tip joint by `degrees` about +z.
void bar_matrices(const SkinnedBar& bar, f32 degrees, Vector<anim::JointMatrix>& out) {
  anim::Pose pose;
  anim::rest_pose(bar.skeleton, pose);
  pose.rotation[1] = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(degrees));
  Vector<Mat4> model(bar.skeleton.joint_count(), Mat4::identity());
  anim::local_to_model(bar.skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  out.resize(bar.skeleton.joint_count());
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(bar.skeleton.inverse_bind.data(), bar.skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(out.data(), out.size()));
}

}  // namespace

// A skinned actor walking: each frame its tip joint bends further and the instance takes a step,
// under a still camera, so a pixel's motion is its bones' and its step's together. Three frames
// with two in flight: the second reads the first's bone matrices out of slot 0's region while it
// draws from slot 1's, and the third the other way round. Fails if the pool's second write reads
// this frame's matrices (the bend's motion, up to pixels, is left as the error), and without the
// moved table (the step's). Drawn by the mesh path and again by the vertex tier's indexed draw
// (`RasterMode::Vertex`, which every card takes when asked), whose visibility buffers are the same.
// A card with no mesh shaders resolves the mesh leg to the vertex tier, which the second leg flies
// anyway, so that leg says it skipped and the vertex leg proves the rest there.
static void walking_actor(const gfx::Device& device, RasterMode raster) {
  ScopedTunable inputs("renderer.reconstruction.inputs", "true");
  SkinnedBar bar;
  build_skinned_bar(bar);
  Rig rig;
  // A bend of the top metre by 40 degrees moves no vertex 0.7 m; a metre of padding holds it.
  REQUIRE_MESSAGE(make_bar_scene(bar, 1.0f, rig.data, rig.error), rig.error);
  RenderSettings settings = plain_settings();
  settings.raster = raster;
  REQUIRE_MESSAGE(rig.finish(device, settings), rig.error);
  if (raster == RasterMode::Hardware && !device.features().mesh_shader) {
    REQUIRE(rig.resolved.vertex_path);
    MESSAGE("mesh skipped: no mesh shaders on this device");
    return;
  }
  REQUIRE(rig.resolved.vertex_path == (raster == RasterMode::Vertex));
  REQUIRE(rig.resolved.deform_pass);
  REQUIRE(rig.renderer.frames_in_flight() == 2);
  const geometry::ClusterMesh& mesh = rig.data.lod.mesh;
  REQUIRE(mesh.skin.size() == mesh.vertices.size());
  const Vector<Vec3> rest = grid_positions(mesh);
  const InstanceJoints runs[1] = {{0u, 2u}};  // the bar's two joints
  // From +z, where a bend about +z is a sideways swing on the picture.
  const Camera camera =
      orbit_camera_at(rig.data.center, rig.data.radius, 30.0f, radians(90.0f), k_orbit_pitch);

  std::string why;
  Shot shots[3];
  Vector<D3> corners[3];
  for (u32 k = 0; k < 3; ++k) {
    if (k > 0) {
      SceneInstance step;
      step.transform.position = Vec3{0.06f * static_cast<f32>(k), 0.0f, 0.02f};
      step.joints = 2;
      step.bounds_padding = 1.0f;
      const u32 index = 0;
      REQUIRE_MESSAGE(rig.renderer.move_instances(std::span<const u32>(&index, 1),
                                                  std::span<const SceneInstance>(&step, 1), &why),
                      why);
    }
    Vector<anim::JointMatrix> matrices;
    bar_matrices(bar, 10.0f + 15.0f * static_cast<f32>(k), matrices);
    FrameDesc frame;
    frame.camera = camera;
    frame.joints = std::span<const anim::JointMatrix>(matrices.data(), matrices.size());
    frame.instance_joints = std::span<const InstanceJoints>(runs, 1);
    frame.history_reset = k == 0;
    REQUIRE_MESSAGE(shoot(rig, frame, shots[k], why), why);
    const gfx::InstanceDesc* placed_now = rig.scene.instance_record(0);
    REQUIRE(placed_now != nullptr);
    Vector<Vec3> skinned(rest.size());
    anim::skin_positions(std::span<const Vec3>(rest.data(), rest.size()),
                         std::span<const geometry::SkinBinding>(mesh.skin.data(), mesh.skin.size()),
                         std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
                         std::span<Vec3>(skinned.data(), skinned.size()));
    corners[k] =
        placed(std::span<const Vec3>(skinned.data(), skinned.size()), *placed_now, shots[k].eye);
  }
  for (u32 k = 1; k < 3; ++k) {
    u32 counted = 0;
    f64 longest = 0.0;
    f64 sine = 1.0;
    const f64 worst = worst_deformed(shots[k], shots[k - 1], 0, mesh, corners[k], corners[k - 1],
                                     counted, longest, sine);
    MESSAGE(std::string(raster_name(raster))
            << ", step " << k << ": worst " << worst << " px (a triangle of "
            << "corner sine " << sine << ") over " << counted << " pixels, the longest vector "
            << longest << " px");
    CHECK(counted > 200);
    CHECK(longest > 1.0);
    CHECK(longest < 16.0);  // the tolerance's premise: a half float's step is 1/128 px under 16
    CHECK(worst <= k_tolerance_px);
  }
}

TEST_CASE("motion: a walking skinned actor's pixels follow its bones and its steps") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  walking_actor(gpu.device, RasterMode::Hardware);
  walking_actor(gpu.device, RasterMode::Vertex);
}

#if ENGINE_RENDERER_TESTS_FLORA
// A young palm in a wind of 1.5 from +x, its sway's clock a quarter second on each frame under a
// still camera: every pixel of it follows its part's two poses. The poses are the poser's own
// function run beside the renderer (`flora_pose_instances`, over a tree laid out as `GpuScene`
// lays it out), and a vertex's bend is `gfx::flora_bend`, the stage's mirror. Three frames with
// two in flight, as for the actor: last frame's poses come out of the other slot's region of the
// part-pose table. Fails if the second write reads this frame's poses.
TEST_CASE("motion: a swaying tree's pixels follow its parts' poses") {
  const test::TempDir tmp("renderer_motion_palm");
  REQUIRE(tmp.ok());
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable inputs("renderer.reconstruction.inputs", "true");
  const std::string path = tmp.file("young.flora.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"species":"date_palm","palm":{"seed":4,"height_m":3.5,"frond_count":10,)"
         R"("leaflet_count":12,"dead_skirt_fronds":3,"triangle_budget":9000}})";
  }
  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, plain_settings()), rig.error);
  REQUIRE(rig.renderer.frames_in_flight() == 2);
  REQUIRE(rig.data.sources.size() == 1);
  const SourceMesh& source = rig.data.sources[0];
  REQUIRE(source.has_motion());
  REQUIRE(rig.scene.flora_stats().instances == 1);
  const geometry::ClusterMesh& mesh = rig.data.lod.mesh;
  REQUIRE(rig.data.parts.size() == 1);
  REQUIRE(rig.data.parts[0].first_vertex == 0);
  REQUIRE(source.motion_vertices.size() == mesh.vertices.size());
  const Vector<Vec3> rest = grid_positions(mesh);

  // The mirror's tree, laid out as `GpuScene::flora_instance` lays the scene's out: the mesh's
  // part table, its rest bounds, the instance's seed from its motion key and its 3x3 inverted.
  FloraMesh tree;
  REQUIRE(flora_mesh(std::span<const geometry::ClusterFileMotionPart>(source.motion_parts.data(),
                                                                      source.motion_parts.size()),
                     std::span<const geometry::ClusterFileStructuralPart>(
                         source.structural_parts.data(), source.structural_parts.size()),
                     source.species, tree));
  Vec3 lo = mesh.vertices[0];
  Vec3 hi = mesh.vertices[0];
  for (const Vec3& v : mesh.vertices) {
    lo = min(lo, v);
    hi = max(hi, v);
  }
  const Vec4 bound{(lo + hi) * 0.5f, length(hi - lo) * 0.5f};
  const gfx::InstanceDesc* placed_tree = rig.scene.instance_record(0);
  REQUIRE(placed_tree != nullptr);
  const gfx::InstanceDesc tree_desc = *placed_tree;
  FloraInstance fi;
  fi.instance = 0;
  fi.mesh = 0;
  fi.first_part = 0;
  const u32 key = rig.data.motion_keys.empty() ? 0u : rig.data.motion_keys[0];
  fi.seed = key * 0x9e3779b9u + 0x632be5abu;
  {
    const Vec3 r0 = tree_desc.rows[0].xyz();
    const Vec3 r1 = tree_desc.rows[1].xyz();
    const Vec3 r2 = tree_desc.rows[2].xyz();
    const Vec3 c0{r0.x, r1.x, r2.x};
    const Vec3 c1{r0.y, r1.y, r2.y};
    const Vec3 c2{r0.z, r1.z, r2.z};
    const f32 det = dot(c0, cross(c1, c2));
    REQUIRE(det != 0.0f);
    const f32 inv = 1.0f / det;
    fi.inverse[0] = cross(c1, c2) * inv;
    fi.inverse[1] = cross(c2, c0) * inv;
    fi.inverse[2] = cross(c0, c1) * inv;
    fi.inverse_scale = tree_desc.scale_max > 0.0f ? 1.0f / tree_desc.scale_max : 1.0f;
  }
  Vector<FloraPartState> state(tree.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(tree.part_count(), gfx::FloraPartPose{});
  FloraPoseInputs in;
  in.instances = std::span<FloraInstance>(&fi, 1);
  in.meshes = std::span<const FloraMesh>(&tree, 1);
  in.bounds = std::span<const Vec4>(&bound, 1);
  in.placed = std::span<const gfx::InstanceDesc>(&tree_desc, 1);
  in.state = std::span<FloraPartState>(state.data(), state.size());
  in.region = poses.data();

  Camera camera;
  camera.position = WorldPos{0.0, 2.0, 7.0};
  camera.target = WorldPos{0.0, 2.0, 0.0};
  FrameFlora wind;
  wind.wind_set = true;
  wind.wind_to = Vec2{-1.0f, 0.0f};
  wind.wind_strength = 1.5f;

  std::string why;
  Shot shots[3];
  Vector<D3> corners[3];
  for (u32 k = 0; k < 3; ++k) {
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = 60 + k;
    frame.world_time_s = 2.0 + 0.25 * static_cast<f64>(k);
    frame.flora = wind;
    frame.history_reset = k == 0;
    REQUIRE_MESSAGE(shoot(rig, frame, shots[k], why), why);
    FloraPoseFrame pose_frame;
    pose_frame.wind_to = wind.wind_to;
    pose_frame.wind_strength = wind.wind_strength;
    pose_frame.time_s = frame.world_time_s;
    pose_frame.frame_index = frame.frame_index;
    pose_frame.flora = &frame.flora;
    (void)flora_pose_instances(in, pose_frame, 0, 1);
    Vector<Vec3> bent(rest.size());
    for (u32 v = 0; v < rest.size(); ++v) {
      const geometry::ClusterFileMotionVertex record = source.motion_vertices[v];
      bent[v] = rest[v];
      if (record.part >= tree.part_count()) continue;  // the stage leaves it at rest
      Vec3 normal;
      gfx::flora_bend(poses[record.part],
                      static_cast<f32>(record.weight) * static_cast<f32>(1.0 / 65535.0), rest[v],
                      Vec3{0.0f, 1.0f, 0.0f}, bent[v], normal);
    }
    corners[k] = placed(std::span<const Vec3>(bent.data(), bent.size()), tree_desc, shots[k].eye);
  }
  for (u32 k = 1; k < 3; ++k) {
    u32 counted = 0;
    f64 longest = 0.0;
    f64 sine = 1.0;
    const f64 worst = worst_deformed(shots[k], shots[k - 1], 0, mesh, corners[k], corners[k - 1],
                                     counted, longest, sine);
    MESSAGE("a quarter second " << k << ": worst " << worst << " px (a triangle of corner sine "
                                << sine << ") over " << counted << " pixels, the longest vector "
                                << longest << " px");
    CHECK(counted > 500);
    CHECK(longest > 1.0);
    CHECK(longest < 16.0);
    CHECK(worst <= k_tolerance_px);
  }
}
#endif  // ENGINE_RENDERER_TESTS_FLORA
