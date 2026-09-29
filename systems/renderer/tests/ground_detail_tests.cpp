// The sand's detail in the renderer (docs/subsystems/renderer.md, "The sand close up"): a terrain
// whose description carries a detail block draws wind ripples and grain on its sand, a function of
// the pixel's world position filtered by its footprint (domain/gfx's ground_detail.slang, held to
// its CPU mirror by domain/gfx's own test). These cases are the renderer's half, on the waves — the
// renderer's own ground, so they run in every configuration, the minimal one included:
//
// - A block that asks for nothing draws what no block draws, byte for byte, and one that asks for
//   the detail changes the terrain's colour and none of its ids.
// - Two runs draw the same bytes; occlusion culling and the cone test change no pixel with the
//   detail on; and the ray-traced path, where the device traces, shades a pixel whose surface it
//   agrees on with the rasterizer to the same byte (they share the resolve).
// - **The filter**: a camera pulling back from a walker's eye height to 350 m over the sand draws
//   no moiré and no pop — the detail adds next to nothing to the frame-to-frame change along the
//   pull-back, and next to nothing to the error against a supersampled picture of the same frame —
//   and distant sand is as bright as the supersampled ripples it stands for, not a mirror.
// - The reference path tracer draws the same sand: at a pixel's centre, close up, it agrees with
// the
//   resolve with the detail on about as well as with it off.
//
// The seams across the rings and their chunks are in ground_detail_rings_tests.cpp, which needs the
// terrain capability's rings.
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/gfx/ground_detail.h>
#include <foundation/image/decode.h>
#include <foundation/image/metrics.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

std::string slashes(const std::filesystem::path& p) { return p.generic_string(); }

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

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height) {
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  bool shoot(const Camera& camera, u64 frame_index, CapturedFrame& out, bool ids = false) {
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = frame_index;
    CaptureChannels channels;
    channels.ids = ids;
    // Twice: the first frame of a renderer has no occlusion history, and a picture is compared
    // with the settled one.
    return renderer.render_offscreen(frame, &error) &&
           renderer.capture(frame, channels, out, &error);
  }
};

// Two kilometres of the waves at 4 m a sample, with a sun low enough to cross the crests: the
// detail at its scene defaults unless `detail` says otherwise.
SceneDesc sand_scene(const std::string& ddc, bool detail,
                     const scene::TerrainDetail& numbers = scene::TerrainDetail{}) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 257;
  desc.terrain.extent = 512.0f;
  desc.terrain.seed = 11;
  desc.terrain.dune_height = 4.0f;
  desc.terrain.dune_wavelength = 120.0f;
  desc.terrain.has_detail = detail;
  desc.terrain.detail = numbers;
  desc.ddc = ddc;
  return desc;
}

RenderSettings sand_settings() {
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.sun_azimuth_deg = 200.0f;
  settings.sun_elevation_deg = 18.0f;
  return settings;
}

// A walker's eyes 1.65 m over the sand at (x, z), looking `ahead` metres along `heading` radians
// and down to the ground there.
Camera walker(const TerrainDesc& terrain, f32 x, f32 z, f32 heading, f32 ahead) {
  Camera camera;
  camera.position = Vec3{x, terrain_height(terrain, x, z) + 1.65f, z};
  const f32 tx = x + ahead * std::cos(heading);
  const f32 tz = z + ahead * std::sin(heading);
  camera.target = Vec3{tx, terrain_height(terrain, tx, tz), tz};
  camera.znear = 0.05f;
  return camera;
}

u32 differing(const CapturedFrame& a, const CapturedFrame& b) {
  u32 n = 0;
  for (u32 p = 0; p < a.width * a.height; ++p)
    n += std::memcmp(&a.color[p * 4], &b.color[p * 4], 3) != 0 ? 1u : 0u;
  return n;
}

f64 luma(const u8* p) {
  return 0.2126 * static_cast<f64>(p[0]) + 0.7152 * static_cast<f64>(p[1]) +
         0.0722 * static_cast<f64>(p[2]);
}

}  // namespace

TEST_CASE("sand detail: nothing asked draws nothing new; the rest is the same bytes every way") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_detail"};
  const std::string ddc = slashes(tmp.native() / "ddc");
  constexpr u32 k_width = 240;
  constexpr u32 k_height = 160;
  scene::TerrainDetail nothing;
  nothing.ripple_height = 0.0f;
  nothing.grain_albedo = 0.0f;
  nothing.grain_roughness = 0.0f;
  const SceneDesc off = sand_scene(ddc, false);
  const SceneDesc zero = sand_scene(ddc, true, nothing);
  const SceneDesc on = sand_scene(ddc, true);
  const Camera camera = walker(off.terrain, 12.0f, -30.0f, 0.4f, 12.0f);

  CapturedFrame shot_off;
  CapturedFrame shot_zero;
  CapturedFrame shot_on;
  CapturedFrame shot_on_again;
  {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, off, sand_settings(), k_width, k_height), rig.error);
    CHECK_FALSE(rig.scene.ground_detail());
    REQUIRE_MESSAGE(rig.shoot(camera, 0, shot_off, true), rig.error);
  }
  {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, zero, sand_settings(), k_width, k_height), rig.error);
    CHECK(rig.scene.ground_detail());
    CHECK(rig.scene.ground_detail_params().flags == 0u);
    REQUIRE_MESSAGE(rig.shoot(camera, 0, shot_zero), rig.error);
  }
  {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, on, sand_settings(), k_width, k_height), rig.error);
    REQUIRE(rig.scene.ground_detail());
    const gfx::GroundDetailParams block = rig.scene.ground_detail_params();
    CHECK(block.flags == (gfx::k_ground_ripples | gfx::k_ground_grain));
    // The waves' wind is one unit vector, whatever the time.
    CHECK(std::abs(block.wind.x * block.wind.x + block.wind.y * block.wind.y - 1.0f) < 1e-5f);
    REQUIRE_MESSAGE(rig.shoot(camera, 0, shot_on, true), rig.error);
    // The ids: the geometry is the same, only its shading is not.
    u32 id_changes = 0;
    for (u32 w = 0; w < shot_on.ids.size(); ++w)
      id_changes += shot_on.ids[w] != shot_off.ids[w] ? 1u : 0u;
    CHECK(id_changes == 0u);
  }
  {
    Rig rig;  // a second, fresh run: the same bytes
    REQUIRE_MESSAGE(rig.build(gpu.device, on, sand_settings(), k_width, k_height), rig.error);
    REQUIRE_MESSAGE(rig.shoot(camera, 0, shot_on_again), rig.error);
  }
  const u32 zero_vs_off = differing(shot_zero, shot_off);
  const u32 on_vs_off = differing(shot_on, shot_off);
  const u32 runs = differing(shot_on, shot_on_again);
  MESSAGE("a block asking for nothing against none: " << zero_vs_off << " pixels differ; the "
                                                      << "detail on against off: " << on_vs_off
                                                      << " of " << shot_on.covered
                                                      << " covered; two runs: " << runs);
  CHECK(zero_vs_off == 0u);
  // Near the camera every pixel; far off, where the ripples and the grain have faded into the
  // roughness, a byte moves only where the transfer carries one.
  CHECK(on_vs_off * 4u > shot_on.covered);
  CHECK(runs == 0u);

  // Occlusion culling and the cone test change no pixel with the detail on (the detail reads the
  // pixel's surface and nothing about how the cut was drawn).
  RenderSettings variants[2] = {sand_settings(), sand_settings()};
  variants[0].occlusion = false;
  variants[1].cone = false;
  for (u32 v = 0; v < 2; ++v) {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, on, variants[v], k_width, k_height), rig.error);
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.shoot(camera, 0, shot), rig.error);
    CHECK_MESSAGE(differing(shot, shot_on) == 0u, (v == 0 ? "occlusion off" : "cones off"));
  }

  // The ray-traced path draws the visibility buffer by rays and shades it with the same resolve:
  // wherever the two agree on the surface, the same byte.
  RenderSettings rays = sand_settings();
  rays.raster = RasterMode::RayTrace;
  Rig rt;
  if (!rt.build(gpu.device, on, rays, k_width, k_height)) {
    MESSAGE("no ray path here: " << rt.error);
    return;
  }
  CapturedFrame shot_rt;
  REQUIRE_MESSAGE(rt.shoot(camera, 0, shot_rt, true), rt.error);
  u32 same_surface = 0;
  u32 same_surface_differs = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    bool same = shot_rt.ids[p * k_id_words] != k_no_id;
    for (u32 w = 0; w < k_id_words; ++w)
      same = same && shot_rt.ids[p * k_id_words + w] == shot_on.ids[p * k_id_words + w];
    if (!same) continue;
    ++same_surface;
    same_surface_differs += std::memcmp(&shot_rt.color[p * 4], &shot_on.color[p * 4], 3) != 0;
  }
  MESSAGE("the ray path: " << same_surface << " pixels on the rasterizer's surface, "
                           << same_surface_differs << " of them another colour");
  CHECK(same_surface * 10u > shot_on.covered * 9u);
  CHECK(same_surface_differs == 0u);
}

TEST_CASE("sand detail: pulling back from the eye to 350 m, no moiré and no pop") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_pullback"};
  const std::string ddc = slashes(tmp.native() / "ddc");
  constexpr u32 k_width = 192;
  constexpr u32 k_height = 108;
  constexpr u32 k_super = 4;  // the supersampled picture's factor, box-filtered down
  constexpr u32 k_frames = 48;
  const SceneDesc desc_on = sand_scene(ddc, true);
  const SceneDesc desc_off = sand_scene(ddc, false);
  // The rig per picture: detail on and off, at the picture's size and four times it.
  Rig on;
  Rig off;
  Rig on_super;
  Rig off_super;
  REQUIRE_MESSAGE(on.build(gpu.device, desc_on, sand_settings(), k_width, k_height), on.error);
  REQUIRE_MESSAGE(off.build(gpu.device, desc_off, sand_settings(), k_width, k_height), off.error);
  REQUIRE_MESSAGE(
      on_super.build(gpu.device, desc_on, sand_settings(), k_width * k_super, k_height * k_super),
      on_super.error);
  REQUIRE_MESSAGE(
      off_super.build(gpu.device, desc_off, sand_settings(), k_width * k_super, k_height * k_super),
      off_super.error);
  // The camera rises from a walker's eyes to 350 m over one point of the sand, backing away from
  // it as it rises, geometrically so every frame is the same step in scale.
  const TerrainDesc& terrain = desc_on.terrain;
  const Vec3 target{40.0f, terrain_height(terrain, 40.0f, 10.0f), 10.0f};
  const auto camera_at = [&](u32 f) {
    const f32 t = static_cast<f32>(f) / static_cast<f32>(k_frames - 1);
    const f32 height = 1.65f * std::pow(350.0f / 1.65f, t);
    Camera camera;
    const f32 back = 2.2f * height + 3.0f;
    const f32 x = target.x - back * 0.8f;
    const f32 z = target.z - back * 0.6f;
    camera.position = Vec3{x, terrain_height(terrain, x, z) + height, z};
    camera.target = target;
    camera.znear = 0.05f;
    return camera;
  };
  // Box-filtered down: the supersampled picture's pixel is the mean of its k x k samples in the
  // display's bytes, which is what an eye averages over too.
  const auto down = [&](const CapturedFrame& big, Vector<f64>& out) {
    out.assign(k_width * k_height, 0.0);
    for (u32 y = 0; y < k_height * k_super; ++y) {
      for (u32 x = 0; x < k_width * k_super; ++x) {
        out[(y / k_super) * k_width + x / k_super] +=
            luma(&big.color[(y * k_width * k_super + x) * 4]);
      }
    }
    for (f64& v : out)
      v /= static_cast<f64>(k_super * k_super);
  };
  Vector<f64> previous_on;
  Vector<f64> previous_off;
  f64 worst_extra_change = 0.0;
  f64 worst_extra_alias = 0.0;
  f64 worst_bias = 0.0;
  f64 sum_extra_change = 0.0;
  f64 sum_extra_alias = 0.0;
  u32 worst_change_frame = 0;
  u32 worst_alias_frame = 0;
  for (u32 f = 0; f < k_frames; ++f) {
    const Camera camera = camera_at(f);
    CapturedFrame a;
    CapturedFrame b;
    CapturedFrame big_a;
    CapturedFrame big_b;
    REQUIRE_MESSAGE(on.shoot(camera, f, a), on.error);
    REQUIRE_MESSAGE(off.shoot(camera, f, b), off.error);
    REQUIRE_MESSAGE(on_super.shoot(camera, f, big_a), on_super.error);
    REQUIRE_MESSAGE(off_super.shoot(camera, f, big_b), off_super.error);
    Vector<f64> ref_on;
    Vector<f64> ref_off;
    down(big_a, ref_on);
    down(big_b, ref_off);
    Vector<f64> la(k_width * k_height);
    Vector<f64> lb(k_width * k_height);
    for (u32 p = 0; p < k_width * k_height; ++p) {
      la[p] = luma(&a.color[p * 4]);
      lb[p] = luma(&b.color[p * 4]);
    }
    // Against the supersampled picture: the mean |error| with the detail and without it, and the
    // mean signed error with it — what the filter leaves too bright or too dark.
    f64 alias_on = 0.0;
    f64 alias_off = 0.0;
    f64 bias = 0.0;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      alias_on += std::abs(la[p] - ref_on[p]);
      alias_off += std::abs(lb[p] - ref_off[p]);
      bias += la[p] - ref_on[p];
    }
    const f64 n = static_cast<f64>(k_width * k_height);
    alias_on /= n;
    alias_off /= n;
    bias /= n;
    const f64 extra_alias = alias_on - alias_off;
    sum_extra_alias += extra_alias;
    if (extra_alias > worst_extra_alias) {
      worst_extra_alias = extra_alias;
      worst_alias_frame = f;
    }
    worst_bias = std::max(worst_bias, std::abs(bias));
    // Frame to frame: the mean |change| with the detail and without it. The camera moves, so both
    // change; what the detail adds is the difference.
    if (f > 0) {
      f64 change_on = 0.0;
      f64 change_off = 0.0;
      for (u32 p = 0; p < k_width * k_height; ++p) {
        change_on += std::abs(la[p] - previous_on[p]);
        change_off += std::abs(lb[p] - previous_off[p]);
      }
      const f64 extra = (change_on - change_off) / n;
      sum_extra_change += extra;
      if (extra > worst_extra_change) {
        worst_extra_change = extra;
        worst_change_frame = f;
      }
    }
    previous_on = la;
    previous_off = lb;
  }
  MESSAGE("pull-back over " << k_frames << " frames at " << k_width << "x" << k_height
                            << ": the detail adds to the frame-to-frame change "
                            << sum_extra_change / (k_frames - 1) << " levels of luma on average, "
                            << worst_extra_change << " at most (frame " << worst_change_frame
                            << "); to the error against a " << k_super << "x" << k_super
                            << " supersampled picture " << sum_extra_alias / k_frames
                            << " on average, " << worst_extra_alias << " at most (frame "
                            << worst_alias_frame << "); its mean bias against it at most "
                            << worst_bias << " levels");
  CHECK(worst_extra_change < 1.5);
  CHECK(worst_extra_alias < 1.0);
  CHECK(worst_bias < 1.5);
}

TEST_CASE("sand detail: the reference path tracer shades the same sand") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_reference"};
  const std::string ddc = slashes(tmp.native() / "ddc");
  constexpr u32 k_width = 160;
  constexpr u32 k_height = 120;
  RenderSettings settings = sand_settings();
  settings.shadows = ShadowMode::RayTraced;
  ReferenceSettings rs;
  rs.spp = 64;
  rs.max_bounces = 1;
  rs.pixel_center = true;  // the resolve's point, so the detail is the same function evaluated
  f32 flip_mean[2] = {};
  for (u32 detail = 0; detail < 2; ++detail) {
    const SceneDesc desc = sand_scene(ddc, detail == 1);
    // At the feet: every ripple a dozen pixels wide, where the resolve's fade is 1 and the two
    // integrators evaluate one function at one point.
    const Camera camera = walker(desc.terrain, -20.0f, 15.0f, 1.1f, 1.2f);
    Rig rig;
    if (!rig.build(gpu.device, desc, settings, k_width, k_height)) {
      MESSAGE("no ray tracing here: " << rig.error);
      return;
    }
    std::string why;
    if (!reference_available(rig.resolved, gpu.device, &why)) {
      MESSAGE("no reference here: " << why);
      return;
    }
    ReferenceRenderer reference;
    REQUIRE_MESSAGE(reference.create(gpu.device, rig.scene, rig.renderer, {}, &rig.error),
                    rig.error);
    ReferenceFrame traced;
    REQUIRE_MESSAGE(reference.render(camera, rs, traced, &rig.error), rig.error);
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.shoot(camera, 0, shot), rig.error);
    image::Image a;
    image::Image b;
    a.width = b.width = k_width;
    a.height = b.height = k_height;
    a.channels = b.channels = 4;
    a.pixels = traced.color;
    b.pixels = shot.color;
    image::FloatImage map;
    REQUIRE(image::flip(a, b, map, image::FlipOptions{}, &rig.error));
    flip_mean[detail] = image::pool_mean(map);
  }
  MESSAGE("resolve against the reference at 64 spp, one bounce, pixel centres, at the feet: FLIP "
          << flip_mean[0] << " without the detail, " << flip_mean[1] << " with it");
  // The same sand in both: the detail adds no more than a small share of what the transport
  // already differs by (a sampled ripple under a jittered bounce is noise the resolve has not).
  CHECK(flip_mean[1] < flip_mean[0] + 0.02f);
}
