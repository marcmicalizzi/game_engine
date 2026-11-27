// A smooth surface reflects the scene (roadmap R71; docs/subsystems/renderer.md, "Reflections on
// smooth surfaces"; surface_reflect.slang). On the tree's own fixture, content/test-scenes/
// glass-street: two walls of glass at roughness 0.06 across a street, a red box behind the eye and
// outside its view, and beside the near wall a honed panel in the fade (0.15) and a rough one past
// it (0.45). Every promise is held by a count of pixels, never by a clock.
#include "test_device.h"

#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdlib>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// The scene file's meshes, in its order.
constexpr u32 k_glass = 0;
constexpr u32 k_fade = 1;
constexpr u32 k_rough = 2;
// The sky's epoch is midnight: the street is drawn at 10:00, the morning sun from the east on the
// red box's face towards the panes, which stand in their own shade.
constexpr f64 k_afternoon_s = 10.0 * 3600.0;

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(renderer_test::device_options(), &error)) {
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

// Puts the switches back whatever a case does.
struct RestoreReflect {
  ~RestoreReflect() {
    (void)tunables::apply_overrides("renderer.reflect.surfaces=1");
    (void)tunables::apply_overrides("renderer.reflect.shell_m=32");
  }
};

struct Street {
  SceneDesc desc;
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  ReferenceRenderer reference;
  Camera camera;
  std::string error;
  bool traced = false;  // the device traces, so the reflection exists here

  bool build(const gfx::Device& device, const std::string& ddc, RenderSettings settings) {
    const std::string path =
        test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/glass-street/scene.json",
                        "content/test-scenes/glass-street/scene.json");
    if (!read_scene_file(path, desc, error)) return false;
    desc.ddc = ddc;
    if (!load_scene(desc, data, error)) return false;
    CameraPath camera_path;
    if (!read_camera_path(desc.camera_path, &desc.terrain, camera_path, error)) return false;
    camera = sample_camera_path(camera_path, 0.0);
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    traced = resolved.shadows;
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = 480;
    rd.height = 270;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  // The mesh a pixel's cluster belongs to, or -1 for the sky.
  i32 mesh_at(const CapturedFrame& shot, u32 p) const {
    const u32 instance = shot.ids[u64{p} * k_id_words];
    if (instance >= data.instances.size()) return -1;
    return static_cast<i32>(data.instances[instance].mesh);
  }

  bool shoot(CapturedFrame& shot) {
    FrameDesc frame;
    frame.camera = camera;
    frame.sky_time_s = k_afternoon_s;
    CaptureChannels channels;
    channels.ids = true;
    // Two frames, so the counts the second reads back are the first's.
    if (!renderer.render_offscreen(frame, &error)) return false;
    return renderer.capture(frame, channels, shot, &error);
  }
};

bool reflective(i32 mesh) {
  return mesh == static_cast<i32>(k_glass) || mesh == static_cast<i32>(k_fade);
}

}  // namespace

TEST_CASE("surface reflect: the panes reflect the red box behind the eye, and nothing else moves") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_reflect");
  REQUIRE(tmp.ok());
  Street street;
  REQUIRE_MESSAGE(street.build(gpu.device, tmp.file("ddc"), RenderSettings{}), street.error);
  // The fixture's materials are what the case is about: the glass and the honed stone are under
  // the fade's end, the rough stone is not.
  CHECK(street.scene.has_reflective_materials());

  CapturedFrame off;
  CapturedFrame on;
  CapturedFrame again;
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
  REQUIRE_MESSAGE(street.shoot(off), street.error);
  CHECK(street.renderer.stats().surface_reflect_rays == 0);
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
  REQUIRE_MESSAGE(street.shoot(on), street.error);
  REQUIRE_MESSAGE(street.shoot(again), street.error);
  if (!street.traced) {
    // The second tier: no ray queries, so the surface draws what it drew (renderer.md).
    CHECK(on.color == off.color);
    CHECK(street.renderer.stats().surface_reflect_rays == 0);
    MESSAGE("this device does not trace: the panes keep the ambient, byte for byte");
    return;
  }
  // With ENGINE_REFLECT_KEEP_CAPTURE set, the two pictures are left in the scratch directory.
  if (!test::detail::environment("ENGINE_REFLECT_KEEP_CAPTURE").empty()) {
    CaptureFiles files;
    std::string kept;
    write_capture(tmp.path(), "street-off", off, files, kept);
    write_capture(tmp.path(), "street-on", on, files, kept);
    tmp.keep();
  }
  const u32 pixels = on.width * on.height;
  u32 glass = 0;
  u32 red_in_glass = 0;
  u32 red_in_glass_off = 0;
  u32 changed = 0;
  u32 changed_elsewhere = 0;
  u32 changed_rough = 0;
  u32 ids_moved = 0;
  for (u32 p = 0; p < pixels; ++p) {
    for (u32 w = 0; w < k_id_words; ++w) {
      if (on.ids[u64{p} * k_id_words + w] != off.ids[u64{p} * k_id_words + w]) {
        ++ids_moved;
        break;
      }
    }
    const i32 mesh = street.mesh_at(on, p);
    const u8* a = &on.color[u64{p} * 4];
    const u8* b = &off.color[u64{p} * 4];
    const bool moved = a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
    if (moved) {
      ++changed;
      if (!reflective(mesh)) ++changed_elsewhere;
      if (mesh == static_cast<i32>(k_rough)) ++changed_rough;
    }
    if (mesh == static_cast<i32>(k_glass)) {
      ++glass;
      // The box's red: red over green by six codes more than the same pixel without the ray,
      // and red over green by eight codes in the picture without it (which no pixel of glass is).
      if ((a[0] - a[1]) - (b[0] - b[1]) >= 6) ++red_in_glass;
      if (b[0] - b[1] >= 8) ++red_in_glass_off;
    }
  }
  const u32 rays = street.renderer.stats().surface_reflect_rays;
  MESSAGE("glass pixels " << glass << ", red in them " << red_in_glass
                          << " (off: " << red_in_glass_off << "), changed " << changed << ", rays "
                          << rays << ", casters " << street.renderer.stats().shadow_casters);
  // The box stands outside the view and is in the panes.
  CHECK(red_in_glass > 100);
  CHECK(red_in_glass_off == 0);
  // Every pixel that changes is a surface under the threshold; the rough stone keeps its ambient.
  CHECK(changed > 0);
  CHECK(changed_elsewhere == 0);
  CHECK(changed_rough == 0);
  CHECK(ids_moved == 0);
  // The rays are counted: at least one a glass pixel (the glass's share is over the floor at every
  // angle), at most one a pixel.
  CHECK(rays >= glass);
  CHECK(rays <= pixels);
  // The same frame twice is the same bytes.
  CHECK(again.color == on.color);
}

TEST_CASE("surface reflect: the eye's shell holds what no view holds") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_shell");
  REQUIRE(tmp.ok());
  // Without the lights' off-frustum volumes, which would hold the box as a caster where it stands
  // in the sun's sweep: what the shell adds is then alone in the difference.
  RenderSettings settings;
  settings.off_frustum_casters = false;
  Street street;
  REQUIRE_MESSAGE(street.build(gpu.device, tmp.file("ddc"), settings), street.error);
  if (!street.traced) {
    MESSAGE("skipped: this device does not trace");
    return;
  }
  // The picture without the ray, then with it and the shell at 0 and at 32 m; and the diagnostic
  // (each traced pixel red where its ray met nothing, green where it met) at both.
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
  CapturedFrame off;
  REQUIRE_MESSAGE(street.shoot(off), street.error);
  struct Count {
    u32 red = 0;  // pane pixels the box's red reached: red over green by ten codes more than off
    u32 met = 0;
    u32 missed = 0;
    u32 casters = 0;
    u32 views = 0;
  };
  const auto count = [&](const char* shell) {
    Count c;
    REQUIRE(tunables::apply_overrides(shell));
    CapturedFrame shot;
    REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
    REQUIRE_MESSAGE(street.shoot(shot), street.error);
    REQUIRE_MESSAGE(street.shoot(shot), street.error);
    for (u32 p = 0; p < shot.width * shot.height; ++p) {
      if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
      const u8* a = &shot.color[u64{p} * 4];
      const u8* b = &off.color[u64{p} * 4];
      if ((a[0] - a[1]) - (b[0] - b[1]) >= 10) ++c.red;
    }
    c.casters = street.renderer.stats().shadow_casters;
    c.views = street.renderer.stats().surface_reflect_views;
    REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=3"));
    REQUIRE_MESSAGE(street.shoot(shot), street.error);
    for (u32 p = 0; p < shot.width * shot.height; ++p) {
      if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
      const u8* a = &shot.color[u64{p} * 4];
      if (a[1] == 255 && a[0] < 200) ++c.met;
      if (a[0] == 255 && a[1] < 200) ++c.missed;
    }
    return c;
  };
  const Count without = count("renderer.reflect.shell_m=0");
  const Count with = count("renderer.reflect.shell_m=32");
  MESSAGE("panes reddened by the box "
          << without.red << " without the shell, " << with.red << " with it; rays met "
          << without.met << " / missed " << without.missed << " without, " << with.met << " / "
          << with.missed << " with; casters " << without.casters << " / " << with.casters);
  CHECK(without.views == 0);
  CHECK(with.views == 1);
  // The box stands behind the eye, outside every view and every light's volume: without the
  // shell the rays that would meet it meet the west panes behind it (so the diagnostic's counts
  // of rays that met something do not move: the frame's cut holds the west panes, whose bounds
  // cross the frustum), with it they meet the box.
  CHECK(without.red == 0);
  CHECK(with.red > 300);
  CHECK(with.casters > without.casters);
  CHECK(with.met >= without.met);
}
TEST_CASE("surface reflect: the mirror pixels agree with the reference path tracer") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_reference");
  REQUIRE(tmp.ok());
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  Street street;
  if (!street.build(gpu.device, tmp.file("ddc"), settings)) {
    MESSAGE("skipped: " << street.error);
    return;
  }
  std::string error;
  if (!reference_available(street.resolved, gpu.device, &error)) {
    MESSAGE("skipped: " << error);
    return;
  }
  REQUIRE_MESSAGE(street.reference.create(gpu.device, street.scene, street.renderer, {}, &error),
                  error);
  // The panes without the ray too, to say what the ray bought.
  CapturedFrame ambient_only;
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
  REQUIRE_MESSAGE(street.shoot(ambient_only), street.error);
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
  CapturedFrame shot;
  REQUIRE_MESSAGE(street.shoot(shot), street.error);
  // Two bounces: the pane, then what it reflects lit by the sun and once by the sky, which is what
  // the reflection's hit is shaded with (`shade_hit`). No jitter, as the agreement case has it.
  ReferenceSettings rs;
  rs.spp = 256;
  rs.batch = 32;
  rs.max_bounces = 2;
  rs.pixel_center = true;
  rs.sky_time_s = k_afternoon_s;
  ReferenceFrame reference;
  REQUIRE_MESSAGE(street.reference.render(street.camera, rs, reference, &error), error);
  REQUIRE(reference.width == shot.width);
  // The mean code value over the panes' pixels, each channel, the two pictures against each other.
  f64 sum_a[3] = {0, 0, 0};
  f64 sum_b[3] = {0, 0, 0};
  f64 abs_diff = 0.0;
  f64 abs_diff_off = 0.0;
  u32 n = 0;
  for (u32 p = 0; p < shot.width * shot.height; ++p) {
    if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
    for (u32 c = 0; c < 3; ++c) {
      const f64 a = shot.color[u64{p} * 4 + c];
      const f64 b = reference.color[u64{p} * 4 + c];
      sum_a[c] += a;
      sum_b[c] += b;
      abs_diff += std::fabs(a - b) / 3.0;
      abs_diff_off += std::fabs(static_cast<f64>(ambient_only.color[u64{p} * 4 + c]) - b) / 3.0;
    }
    ++n;
  }
  REQUIRE(n > 0);
  MESSAGE("panes: " << n << " pixels, mean codes resolve (" << sum_a[0] / n << ", " << sum_a[1] / n
                    << ", " << sum_a[2] / n << ") reference (" << sum_b[0] / n << ", "
                    << sum_b[1] / n << ", " << sum_b[2] / n << "), mean |difference| "
                    << abs_diff / n << " (without the ray " << abs_diff_off / n << ")");
  // The bounds are the measurement with room for driver noise and another card, not targets: on
  // the RTX 5090 (2026-11-26, the panes in their own shade at 10:00) the channel means were 4.8,
  // 5.9 and 6.8 codes apart, the resolve the brighter, and the mean difference 5.9 codes against
  // 10.2 without the ray. What is left is the hit's shading, not the ray: `shade_hit` lights what
  // the ray met with the sky's whole harmonics where the cache is off (as here), where the
  // reference sees the street's walls take part of that sky and adds the street's bounce; and the
  // 256 samples' noise. The ray itself is the reference's lobe at 0.06, well under a pixel wide.
  for (u32 c = 0; c < 3; ++c)
    CHECK(std::fabs(sum_a[c] - sum_b[c]) / n < 9.0);
  CHECK(abs_diff / n < 8.0);
  // And the ray brings the panes nearer the reference than the ambient did.
  CHECK(abs_diff < abs_diff_off);
}
