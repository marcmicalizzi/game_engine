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
//   the resolve with the detail on about as well as with it off — the first pass on the waves as
//   they are, and the second and third passes' terms (the ergs' numbers) at the feet on steeper
//   waves, on a windward slope and on a lee in the grainflow's band.
//
// The seams across the rings and their chunks are in ground_detail_rings_tests.cpp, which needs the
// terrain capability's rings.
#include "ground_detail_reference.h"

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

namespace {

// The resolve against the reference path tracer at `camera`, traced shadows, 64 samples a pixel
// at the pixel's centre (the resolve's point, so the detail is the same function evaluated), one
// bounce: FLIP's pooled mean between the two pictures, and in `halves` its mean over the picture's
// lower half (the ground nearest the feet) and its upper half. False, with the reason in `why`,
// where the device cannot trace or has no reference.
bool flip_against_reference(const gfx::Device& device, const SceneDesc& desc, const Camera& camera,
                            f32& flip, std::string& why, f32* halves = nullptr) {
  constexpr u32 k_width = 160;
  constexpr u32 k_height = 120;
  RenderSettings settings = sand_settings();
  settings.shadows = ShadowMode::RayTraced;
  ReferenceSettings rs;
  rs.spp = 64;
  rs.max_bounces = 1;
  rs.pixel_center = true;
  Rig rig;
  if (!rig.build(device, desc, settings, k_width, k_height)) {
    why = "no ray tracing here: " + rig.error;
    return false;
  }
  if (!reference_available(rig.resolved, device, &why)) {
    why = "no reference here: " + why;
    return false;
  }
  ReferenceRenderer reference;
  REQUIRE_MESSAGE(reference.create(device, rig.scene, rig.renderer, {}, &rig.error), rig.error);
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
  flip = image::pool_mean(map);
  if (halves != nullptr) {
    f64 sum[2] = {};
    for (u32 y = 0; y < k_height; ++y) {
      for (u32 x = 0; x < k_width; ++x)
        sum[y < k_height / 2 ? 1 : 0] += static_cast<f64>(map.values[y * k_width + x]);
    }
    for (u32 h = 0; h < 2; ++h)
      halves[h] = static_cast<f32>(sum[h] / static_cast<f64>(k_width * (k_height / 2)));
  }
  return true;
}

// The ergs' numbers (`ground_ref::erg_numbers`, the desert-erg scene's `terrain.detail`) as a
// scene's block: every term the ergs draw. The rings' test holds the copy to the scene file.
scene::TerrainDetail erg_detail() {
  const gfx::GroundDetailDesc d = ground_ref::erg_numbers();
  scene::TerrainDetail t;
  t.ripple_wavelength = d.ripple_wavelength;
  t.ripple_height = d.ripple_height;
  t.ripple_asymmetry = d.ripple_asymmetry;
  t.ripple_defects = d.ripple_defects;
  t.ripple_slope_start_deg = d.slope_start_deg;
  t.ripple_slope_end_deg = d.slope_end_deg;
  t.grain_size = d.grain_size;
  t.grain_albedo = d.grain_albedo;
  t.grain_roughness = d.grain_roughness;
  t.lee_start_deg = d.lee_start_deg;
  t.lee_end_deg = d.lee_end_deg;
  t.grain_finest = d.grain_finest;
  t.grain_normal = d.grain_normal;
  t.streak_start_deg = d.streak_start_deg;
  t.streak_full_deg = d.streak_full_deg;
  t.streak_width = d.streak_width;
  t.streak_length = d.streak_length;
  t.streak_albedo = d.streak_albedo;
  t.streak_roughness = d.streak_roughness;
  t.streak_normal = d.streak_normal;
  t.spacing_gain = d.spacing_gain;
  t.spacing_min = d.spacing_min;
  t.spacing_max = d.spacing_max;
  t.flow_start_deg = d.flow_start_deg;
  t.flow_full_deg = d.flow_full_deg;
  t.flow_cell = d.flow_cell;
  t.flow_width = d.flow_width;
  t.flow_normal = d.flow_normal;
  t.flow_albedo = d.flow_albedo;
  t.flow_widening = d.flow_widening;
  t.flow_share = d.flow_share;
  t.flow_turnover = d.flow_turnover;
  t.patch_size = d.patch_size;
  t.patch_min = d.patch_min;
  t.patch_max = d.patch_max;
  t.patch_defects = d.patch_defects;
  t.steer_max_deg = d.steer_max_deg;
  t.steer_gain = d.steer_gain;
  t.ripple_celerity = d.ripple_celerity;
  t.flatten_start = d.flatten_start;
  t.flatten_end = d.flatten_end;
  return t;
}

// Where on `terrain` the ground falls along `wind` nearest `fall_deg` (negative: it climbs into
// the wind) — the fall ground_detail.slang reads, `dot(n.xz, w) / n.y`, as an angle — on a 2 m
// grid within 100 m of the origin, among the points whose fall a metre either way along and across
// the wind stays within a degree of their own: a patch a walker's feet see as one slope. Returns
// the fall found there, degrees.
f64 find_slope(const TerrainDesc& terrain, Vec2 wind, f64 fall_deg, f32& x_out, f32& z_out) {
  const TerrainSampler sampler(terrain);
  const f64 wx = static_cast<f64>(wind.x);
  const f64 wz = static_cast<f64>(wind.y);
  const auto fall = [&](f64 x, f64 z) {
    constexpr f64 e = 0.25;
    const auto h = [&](f64 px, f64 pz) {
      return static_cast<f64>(sampler.height(static_cast<f32>(px), static_cast<f32>(pz)));
    };
    const f64 hx = (h(x + e, z) - h(x - e, z)) / (2.0 * e);
    const f64 hz = (h(x, z + e) - h(x, z - e)) / (2.0 * e);
    return std::atan(-(hx * wx + hz * wz)) * 180.0 / 3.14159265358979323846;
  };
  f64 best = 1e9;
  f64 found = 0.0;
  for (i32 j = -50; j <= 50; ++j) {
    for (i32 i = -50; i <= 50; ++i) {
      const f64 x = 2.0 * i;
      const f64 z = 2.0 * j;
      const f64 here = fall(x, z);
      if (std::abs(here - fall_deg) >= best) continue;
      bool uniform = true;
      for (const f64 s : {-1.0, 1.0}) {
        uniform = uniform && std::abs(fall(x + s * wx, z + s * wz) - here) < 1.0 &&
                  std::abs(fall(x - s * wz, z + s * wx) - here) < 1.0;
      }
      if (!uniform) continue;
      best = std::abs(here - fall_deg);
      found = here;
      x_out = static_cast<f32>(x);
      z_out = static_cast<f32>(z);
    }
  }
  return found;
}

}  // namespace

TEST_CASE("sand detail: the reference path tracer shades the same sand") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_reference"};
  const std::string ddc = slashes(tmp.native() / "ddc");
  f32 flip_mean[2] = {};
  for (u32 detail = 0; detail < 2; ++detail) {
    const SceneDesc desc = sand_scene(ddc, detail == 1);
    // At the feet: every ripple a dozen pixels wide, where the resolve's fade is 1 and the two
    // integrators evaluate one function at one point.
    const Camera camera = walker(desc.terrain, -20.0f, 15.0f, 1.1f, 1.2f);
    std::string why;
    if (!flip_against_reference(gpu.device, desc, camera, flip_mean[detail], why)) {
      MESSAGE(why);
      return;
    }
  }
  MESSAGE("resolve against the reference at 64 spp, one bounce, pixel centres, at the feet: FLIP "
          << flip_mean[0] << " without the detail, " << flip_mean[1] << " with it");
  // The same sand in both: the detail adds no more than a small share of what the transport
  // already differs by (a sampled ripple under a jittered bounce is noise the resolve has not).
  CHECK(flip_mean[1] < flip_mean[0] + 0.02f);
}

TEST_CASE("sand detail: the reference path tracer shades the ergs' sand on a slope") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_reference_slope"};
  const std::string ddc = slashes(tmp.native() / "ddc");
  // The waves steeper than the flat case's — 16 m over 60 m, whose flanks reach a slip face's
  // slopes — with the ergs' numbers, every term they draw. Two walkers' feet: on ground climbing
  // into the wind at about 12 degrees (the spacing, the patches, the ripples and the grain's
  // normal) and on ground falling away from it at about 26 (no ripples, which the exposure takes,
  // and the grainflow's band, 24 to 30; the second pass's streaks, which the ergs drew here until
  // the third pass, have gfx's own case), each against the same place without the detail.
  //
  // **On a grid of a metre, not the flat case's four.** Looking down at the feet, the camera's
  // plane meets the ground two or three metres behind the walker, and a 4 m triangle under the
  // feet reaches past it. The rasterizer clips such a triangle and draws it rightly, but the
  // resolve rebuilds a pixel's point from the corners divided by w (`reconstruct_screen`), which
  // is meaningless for a corner behind the camera, so it shades a point the pixel does not see —
  // the limit gfx's own case builds its sand in metre cells to stay clear of. The second pass's
  // tongues showed it: 2 m long, on the 4 m grid they ran on as stripes to the bottom of the
  // resolved picture, where the path tracer drew them ending, and FLIP rose by 0.018 with the
  // detail, 0.015 of it theirs; on the metre grid the two pictures drew the same tongues
  // (renderer.md, "The sand close up").
  SceneDesc on = sand_scene(ddc, true, erg_detail());
  on.terrain.dune_height = 16.0f;
  on.terrain.dune_wavelength = 60.0f;
  on.terrain.extent = 128.0f;  // 257 vertices a side: a metre
  SceneDesc off = on;
  off.terrain.has_detail = false;
  const Vec2 wind = TerrainSampler(on.terrain).wind(on.terrain.time_s);
  const f32 heading = std::atan2(wind.y, wind.x);  // down the wind
  for (const f64 target : {-12.0, 26.0}) {
    f32 x = 0.0f;
    f32 z = 0.0f;
    const f64 fall = find_slope(on.terrain, wind, target, x, z);
    REQUIRE_MESSAGE(std::abs(fall - target) < 1.5, "no ground falling " << target << " degrees");
    const Camera camera = walker(on.terrain, x, z, heading, 1.2f);
    f32 flip_off = 0.0f;
    f32 flip_on = 0.0f;
    f32 halves_off[2] = {};
    f32 halves_on[2] = {};
    std::string why;
    if (!flip_against_reference(gpu.device, off, camera, flip_off, why, halves_off) ||
        !flip_against_reference(gpu.device, on, camera, flip_on, why, halves_on)) {
      MESSAGE(why);
      return;
    }
    MESSAGE("the ergs' numbers at the feet at ("
            << x << ", " << z << "), falling " << fall << " degrees along the wind: FLIP "
            << flip_off << " without the detail, " << flip_on << " with it (the lower half "
            << halves_off[0] << " and " << halves_on[0] << ", the upper " << halves_off[1]
            << " and " << halves_on[1] << ")");
    // The same sand in both: with the third pass the detail adds 0.002 climbing into the wind and
    // 0.001 on the lee (RTX 5090, 2026-09-30) — on the lee nearly all of it in the picture's upper
    // half (0.003 there, under 0.001 in the lower), ten metres down the slope, where the resolve
    // fades a lane by its footprint and a reference at the pixel's centre does not. The second
    // pass's streaks added 0.003 there. Held to half the flat case's allowance.
    CHECK(flip_on < flip_off + 0.01f);
  }
}
