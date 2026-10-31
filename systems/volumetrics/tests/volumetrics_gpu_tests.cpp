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
#include <test_paths.h>
#include <test_temp_dir.h>

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
  f32 moon_age_days = 0.0f;
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
      entry.moon_age_days = moon_age_days;
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
  frame.sky_time_s = time_s;
  frame.world_time_s = time_s;  // the clocks agree here, as the one clock did (R88)
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

// ---- the far clouds in exposed units (volumetrics.md, "The march"; "Testing") -------------------
//
// The march's texels and the froxels are four halves each. Stored as raw radiance in sun units, a
// full moon's cloud (about 1e-7) is a few of a half's denormal steps, which the exposure then
// scales into colour bands, and a low sun's forward lobe can leave the half's range. Stored in
// exposed units they sit where a display wants them. Three hours of one frame: the night's banding
// figure and the sunset's clipped pixels, and noon held to its picture.

namespace {

struct Picture {
  CapturedFrame bare;    // the sky alone
  CapturedFrame clouds;  // the same frame with the far layer
};

struct CloudFigures {
  u32 cloud_pixels = 0;  // pixels the layer moved by more than two codes
  u32 clipped = 0;       // of the picture with the layer, pixels with a channel at 255
  u32 clipped_bare = 0;  // the same of the sky alone: the sun's disc
  u32 run = 0;           // the longest run of cloud along one row, which the next three read
  u32 distinct = 0;      // distinct colours along it
  u32 max_step = 0;      // the largest step between neighbours along it, any channel, codes
  u32 sky_max_step = 0;  // the same pixels of the sky alone: the dither's noise
  u32 sky_distinct = 0;
  f32 chroma_step = 0.0f;      // the banding figure, below: the layer's
  f32 sky_chroma_step = 0.0f;  // the same windows of the sky alone
};

u32 channel_step(const CapturedFrame& f, u32 a, u32 b) {
  u32 m = 0;
  for (u32 c = 0; c < 3; ++c) {
    const int d = int(f.color[a * 4 + c]) - int(f.color[b * 4 + c]);
    m = std::max(m, u32(d < 0 ? -d : d));
  }
  return m;
}

u32 distinct_along(const CapturedFrame& f, u32 row, u32 x0, u32 x1) {
  Vector<u32> seen;
  for (u32 x = x0; x < x1; ++x) {
    const u32 i = (row * f.width + x) * 4;
    const u32 rgb = u32(f.color[i]) | u32(f.color[i + 1]) << 8 | u32(f.color[i + 2]) << 16;
    bool found = false;
    for (u32 s : seen)
      found = found || s == rgb;
    if (!found) seen.push_back(rgb);
  }
  return static_cast<u32>(seen.size());
}

CloudFigures figures(const Picture& p) {
  CloudFigures out;
  const u32 w = p.bare.width;
  const u32 h = p.bare.height;
  Vector<u8> mask;
  mask.resize(usize{w} * h);
  for (u32 i = 0; i < w * h; ++i) {
    u32 moved = 0;
    for (u32 c = 0; c < 3; ++c) {
      const int d = int(p.clouds.color[i * 4 + c]) - int(p.bare.color[i * 4 + c]);
      moved = std::max(moved, u32(d < 0 ? -d : d));
    }
    const bool cloud = moved > 2;
    mask[i] = cloud ? 1 : 0;
    out.cloud_pixels += cloud ? 1u : 0u;
    bool clip = false;
    bool clip_bare = false;
    for (u32 c = 0; c < 3; ++c) {
      clip = clip || p.clouds.color[i * 4 + c] == 255;
      clip_bare = clip_bare || p.bare.color[i * 4 + c] == 255;
    }
    out.clipped += clip ? 1u : 0u;
    out.clipped_bare += clip_bare ? 1u : 0u;
  }
  // The longest run of cloud on any row, less two pixels at either end (the layer's edge).
  u32 best_row = 0, best_x0 = 0, best_x1 = 0;
  for (u32 y = 0; y < h; ++y) {
    u32 x = 0;
    while (x < w) {
      while (x < w && mask[y * w + x] == 0)
        ++x;
      const u32 x0 = x;
      while (x < w && mask[y * w + x] != 0)
        ++x;
      if (x - x0 > best_x1 - best_x0) {
        best_row = y;
        best_x0 = x0;
        best_x1 = x;
      }
    }
  }
  // The banding figure: along every row, the means of eight-pixel windows wholly inside the layer
  // (the dither averaged away), and the largest step of their chroma — red less green, green less
  // blue — between neighbouring windows, in codes. A band of the half's staircase is a step of
  // hue; a smooth gradient's chroma moves by a fraction of a code a window.
  for (u32 y = 0; y < h; ++y) {
    bool have = false;
    f32 prev[2] = {};
    f32 prev_sky[2] = {};
    for (u32 x0 = 0; x0 + 8 <= w; x0 += 8) {
      bool inside = true;
      for (u32 x = x0; x < x0 + 8; ++x)
        inside = inside && mask[y * w + x] != 0;
      if (!inside) {
        have = false;
        continue;
      }
      f32 m[3] = {}, s[3] = {};
      for (u32 x = x0; x < x0 + 8; ++x) {
        for (u32 c = 0; c < 3; ++c) {
          m[c] += f32(p.clouds.color[(y * w + x) * 4 + c]) / 8.0f;
          s[c] += f32(p.bare.color[(y * w + x) * 4 + c]) / 8.0f;
        }
      }
      const f32 chroma[2] = {m[0] - m[1], m[1] - m[2]};
      const f32 chroma_sky[2] = {s[0] - s[1], s[1] - s[2]};
      if (have) {
        for (u32 c = 0; c < 2; ++c) {
          out.chroma_step = std::max(out.chroma_step, std::fabs(chroma[c] - prev[c]));
          out.sky_chroma_step =
              std::max(out.sky_chroma_step, std::fabs(chroma_sky[c] - prev_sky[c]));
        }
      }
      have = true;
      for (u32 c = 0; c < 2; ++c) {
        prev[c] = chroma[c];
        prev_sky[c] = chroma_sky[c];
      }
    }
  }
  if (best_x1 - best_x0 > 8) {
    const u32 x0 = best_x0 + 2;
    const u32 x1 = best_x1 - 2;
    out.run = x1 - x0;
    out.distinct = distinct_along(p.clouds, best_row, x0, x1);
    out.sky_distinct = distinct_along(p.bare, best_row, x0, x1);
    for (u32 x = x0 + 1; x < x1; ++x) {
      const u32 a = best_row * w + x - 1;
      out.max_step = std::max(out.max_step, channel_step(p.clouds, a, a + 1));
      out.sky_max_step = std::max(out.sky_max_step, channel_step(p.bare, a, a + 1));
    }
  }
  return out;
}

// The time of the scene's day at which the sun stands `elevation_deg` above the horizon in the
// afternoon, by bisection over the provider's own ephemeris.
f64 afternoon_at(const Rig& rig, f32 elevation_deg) {
  f64 lo = 12.0 * 3600.0;
  f64 hi = 21.0 * 3600.0;
  const f32 target = std::sin(elevation_deg * 3.14159265f / 180.0f);
  for (u32 i = 0; i < 40; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    FrameSky sky;
    rig.renderer.sky().evaluate(rig.scene.epoch_s() + mid, sky);
    if (sky.state.sun.y > target)
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

FrameDesc towards(f64 time_s, Vec3 dir) {
  FrameDesc frame;
  const Vec3 eye{0.0f, 2.0f, 0.0f};
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + normalize(dir) * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.sky_time_s = time_s;
  frame.world_time_s = time_s;  // the clocks agree here, as the one clock did (R88)
  return frame;
}

// One frame with and without the layer, the layer drawn alone (the froxels off), twice to the
// byte. Kept, with `ENGINE_VOLUMETRICS_KEEP_CAPTURES` set, as PNGs in the test's scratch.
bool picture(const gfx::Device& device, Rig& rig, const FrameDesc& frame, const char* name,
             engine::test::TempDir& tmp, Picture& out) {
  if (!shoot(rig, frame, out.bare)) return false;
  MediumDesc desc;
  desc.clouds = true;
  desc.cloud_coverage = 0.5f;
  MediumOptions options;
  options.debug = MediumDebug::clouds;
  MediumPass pass;
  if (!pass.create(device, rig.renderer, rig.data, desc, options, &rig.error)) return false;
  CapturedFrame again;
  if (!shoot(rig, frame, out.clouds) || !shoot(rig, frame, again)) return false;
  CHECK_MESSAGE(again.color == out.clouds.color, name);
  pass.destroy();
  if (!engine::test::detail::environment("ENGINE_VOLUMETRICS_KEEP_CAPTURES").empty()) {
    CaptureFiles files;
    std::string error;
    write_capture(tmp.path(), std::string(name) + "-bare", out.bare, files, error);
    write_capture(tmp.path(), std::string(name) + "-clouds", out.clouds, files, error);
    tmp.keep();
  }
  return true;
}

void report(const char* name, const CloudFigures& f) {
  MESSAGE(std::string(name) << ": cloud " << f.cloud_pixels << " px, clipped " << f.clipped
                            << " (sky alone " << f.clipped_bare << "), along " << f.run
                            << " px of one row " << f.distinct << " colours, max step "
                            << f.max_step << " (sky alone " << f.sky_distinct
                            << " colours, max step " << f.sky_max_step << "), chroma step "
                            << f.chroma_step << " (sky alone " << f.sky_chroma_step << ")");
}

}  // namespace

TEST_CASE("volumetrics: the far clouds are stored exposed: no bands at night, no clip at sunset") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  rig.moon_age_days = 14.8f;  // full at the epoch's night
  REQUIRE_MESSAGE(rig.build(gpu.device, true, ShadowMode::Off), rig.error);
  engine::test::TempDir tmp("volumetrics-clouds");

  // Night: 23:00, looking up towards the full moon's side of the sky.
  FrameSky night_sky;
  const f64 night_s = 23.0 * 3600.0;
  rig.renderer.sky().evaluate(rig.scene.epoch_s() + night_s, night_sky);
  const Vec3 moon = night_sky.state.moon;
  Picture night;
  REQUIRE_MESSAGE(
      picture(gpu.device, rig, towards(night_s, Vec3{moon.x, 0.2f, moon.z}), "night", tmp, night),
      rig.error);
  // Sunset: the sun 2 degrees up, looking towards it; and dusk, 2 degrees under, the same way.
  const auto low_sun = [&](f32 elevation_deg, const char* name, Picture& out) {
    const f64 time_s = afternoon_at(rig, elevation_deg);
    FrameSky sky;
    rig.renderer.sky().evaluate(rig.scene.epoch_s() + time_s, sky);
    const Vec3 sun = sky.state.sun;
    return picture(gpu.device, rig, towards(time_s, Vec3{sun.x, 0.15f, sun.z}), name, tmp, out);
  };
  Picture sunset;
  REQUIRE_MESSAGE(low_sun(2.0f, "sunset", sunset), rig.error);
  Picture dusk;
  REQUIRE_MESSAGE(low_sun(-2.0f, "dusk", dusk), rig.error);
  // Noon, looking up.
  Picture noon;
  REQUIRE_MESSAGE(
      picture(gpu.device, rig, towards(12.0 * 3600.0, Vec3{0.35f, 0.3f, -1.0f}), "noon", tmp, noon),
      rig.error);

  const CloudFigures n = figures(night);
  const CloudFigures s = figures(sunset);
  const CloudFigures k = figures(dusk);
  const CloudFigures d = figures(noon);
  report("night", n);
  report("sunset", s);
  report("dusk", k);
  report("noon", d);
  if (!engine::test::detail::environment("ENGINE_VOLUMETRICS_KEEP_CAPTURES").empty())
    MESSAGE("kept in " << tmp.path());
  CHECK(n.cloud_pixels > 500);
  CHECK(s.cloud_pixels > 500);
  CHECK(k.cloud_pixels > 500);
  CHECK(d.cloud_pixels > 500);
  // The night's layer has no step of hue the sky beside it does not: measured 1.0 against the
  // sky's 1.1, where the raw halves gave 7.75.
  CHECK(n.chroma_step <= n.sky_chroma_step + 1.0f);
  // No pixel clips that the sky alone does not (the sun's and the moon's discs and their glow):
  // the layer only ever covers them.
  for (const CloudFigures* f : {&n, &s, &k, &d})
    CHECK(f->clipped <= f->clipped_bare);
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
  const MediumFire f = medium_fire(fire, WorldPos{0.0, 0.0, 0.0}, wind, 0.0);
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
  CHECK(smoke_density(medium_fire(clean, WorldPos{}, wind, 0.0), Vec3{10.0f, 0.8f, 0.0f}) == 0.0f);
  // The flame's noise rises with the clock, reduced to its period: finite years in.
  const MediumFire late = medium_fire(fire, WorldPos{}, wind, 94687200.0);
  CHECK(late.flame.z >= 0.0f);
  CHECK(late.flame.z < k_flame_noise_period);
}

TEST_CASE("volumetrics: the flame puffs at Cetegen and Ahmed's frequency") {
  // f = 1.5 / sqrt(D): 1.875 Hz for the campfire's 0.64 m base, its features rising a diameter a
  // puff (1.2 m/s), so the noise's lattice rises two cells up (a feature) once a puff. R85's eye-
  // tuned 1.4 m/s through cells of 0.45 r rose 5.2 cells a puff: a tongue every 0.1 s.
  renderer::FrameFire fire;
  fire.radius_m = 0.32f;
  fire.height_m = 0.85f;
  const f32 f = flame_puff_hz(fire.radius_m);
  CHECK(f == doctest::Approx(1.875).epsilon(1e-4));
  CHECK(flame_rise_m_s(fire.radius_m) == doctest::Approx(1.2).epsilon(1e-4));
  // Halving the diameter puffs sqrt(2) times as often.
  CHECK(flame_puff_hz(0.16f) / f == doctest::Approx(std::sqrt(2.0)).epsilon(1e-4));
  const MediumWind wind;
  const f64 period = 1.0 / static_cast<f64>(f);
  const f64 t0 = 1000.0;
  const MediumFire a = medium_fire(fire, WorldPos{}, wind, t0);
  const MediumFire b = medium_fire(fire, WorldPos{}, wind, t0 + period);
  f64 rose = static_cast<f64>(b.flame.z) - static_cast<f64>(a.flame.z);
  if (rose < 0.0) rose += static_cast<f64>(k_flame_noise_period);
  MESSAGE("the noise rose " << rose << " cells in one puff of " << period << " s");
  CHECK(rose == doctest::Approx(2.0).epsilon(1e-3));
  CHECK(a.wind.w == doctest::Approx(static_cast<f64>(k_flame_cell_up * fire.radius_m)));
}

TEST_CASE("volumetrics: the marched flame hands over to the froxels' blob smoothly") {
  const f32 r = 0.32f;
  const f64 far = static_cast<f64>(k_fire_march_widths) * 3.2 * static_cast<f64>(r);
  CHECK(fire_march_weight(0.0, r) == 1.0f);
  CHECK(fire_march_weight(8.5, r) == 1.0f);  // past R85's step at 8 m the flame is still marched
  CHECK(fire_march_weight(far * 0.74, r) == 1.0f);
  CHECK(fire_march_weight(far, r) == 0.0f);
  CHECK(fire_march_weight(far * 2.0, r) == 0.0f);
  // Monotone, and no step larger than the band's slope allows: 1,000 samples across it.
  f32 last = 1.0f;
  f32 largest = 0.0f;
  for (u32 i = 0; i <= 1000; ++i) {
    const f64 d = far * (0.7 + 0.35 * static_cast<f64>(i) / 1000.0);
    const f32 w = fire_march_weight(d, r);
    CHECK(w <= last);
    largest = std::max(largest, last - w);
    last = w;
  }
  CHECK(largest < 0.01f);
  // The block's share rides in the fire's record: a fire 20 m out is marched whole.
  renderer::FrameFire fire;
  fire.base = WorldPos{20.0, 0.0, 0.0};
  fire.radius_m = r;
  fire.height_m = 0.85f;
  const MediumFire m = medium_fire(fire, WorldPos{}, MediumWind{}, 0.0);
  CHECK(m.emission.w == 1.0f);
  CHECK(m.ids[1] == 1u);
  fire.base = WorldPos{far + 1.0, 0.0, 0.0};
  const MediumFire n = medium_fire(fire, WorldPos{}, MediumWind{}, 0.0);
  CHECK(n.emission.w == 0.0f);
  CHECK(n.ids[1] == 0u);
}

TEST_CASE("volumetrics: a froxel holds the smoke's mean over its depth, wherever its slices fall") {
  renderer::FrameFire fire;
  fire.base = WorldPos{0.0, 0.0, 12.0};
  fire.radius_m = 0.32f;
  fire.height_m = 0.85f;
  fire.smoke_density = 0.5f;
  fire.smoke_rise_m_s = 1.2f;
  fire.smoke_spread = 0.22f;
  MediumWind calm;
  calm.speed_mps = 0.0f;
  MediumWind windy;  // the record's mean towards +x
  for (const MediumWind& wind : {calm, windy}) {
    const MediumFire f = medium_fire(fire, WorldPos{}, wind, 0.0);
    // Rays from the eye at the origin towards the column a little over the flame's top, slightly
    // off its axis and up through it.
    const Vec3 dirs[3] = {normalize(Vec3{0.0f, 1.0f, 12.0f}), normalize(Vec3{0.08f, 1.1f, 12.0f}),
                          normalize(Vec3{0.3f, 1.6f, 12.0f})};
    for (const Vec3& dir : dirs) {
      // The closed form against a fine sum of the point density, over cells 0.2 to 2 m deep.
      for (const f32 depth : {0.2f, 0.7f, 1.6f}) {
        const f32 t0 = 12.0f - 0.4f * depth;
        const f32 t1 = t0 + depth;
        const Vec3 at = dir * (0.5f * (t0 + t1));
        const f32 cell = smoke_cell(f, dir, t0, t1, at, 0.0f);
        f64 sum = 0.0;
        const u32 n = 4000;
        for (u32 i = 0; i < n; ++i) {
          const f32 t = t0 + (static_cast<f32>(i) + 0.5f) * depth / static_cast<f32>(n);
          // The column as it stands at the cell's centre: its radius and fade there (the cell
          // freezes them; the sum uses the point density, which moves them a little).
          sum += static_cast<f64>(smoke_density(f, dir * t));
        }
        const f64 mean = sum / n;
        MESSAGE("depth " << depth << ": cell " << cell << ", sum " << mean);
        CHECK(static_cast<f64>(cell) == doctest::Approx(mean).epsilon(0.08).scale(0.02));
      }
      // Tiled by slices 1.6 m deep at eight offsets, the column's optical depth along the ray is
      // the same wherever the boundaries fall: a point sample a slice took caught all or nothing.
      // The cell freezes the column's radius and fade at its centre, which a ray climbing through
      // it does not see: measured 0.5% to 12% apart across these rays, against R85's point
      // sample, which caught the column whole or missed it.
      f64 lo = 1e30, hi = 0.0, point_lo = 1e30, point_hi = 0.0;
      for (u32 k = 0; k < 8; ++k) {
        f64 tau = 0.0, point = 0.0;
        for (f32 t0 = 8.0f + 0.2f * static_cast<f32>(k); t0 < 16.0f; t0 += 1.6f) {
          const f32 t1 = t0 + 1.6f;
          const Vec3 at = dir * (0.5f * (t0 + t1));
          tau += static_cast<f64>(smoke_cell(f, dir, t0, t1, at, 0.0f)) * 1.6;
          point += static_cast<f64>(smoke_density(f, at)) * 1.6;
        }
        lo = std::min(lo, tau);
        hi = std::max(hi, tau);
        point_lo = std::min(point_lo, point);
        point_hi = std::max(point_hi, point);
      }
      MESSAGE("optical depth across eight slicings " << lo << " to " << hi << "; a point sample "
                                                     << point_lo << " to " << point_hi);
      if (hi > 1e-4) {
        CHECK(hi / lo < 1.15);
        CHECK(hi / lo < point_hi / std::max(point_lo, 1e-9));
      }
    }
  }
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
    later.world_time_s = 0.5;  // the flame's noise is on the world clock (R88)
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

namespace {

// A code of the 8-bit sRGB picture as linear light, 0 to 1.
f64 linear(u8 code) {
  const f64 c = static_cast<f64>(code) / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// The light the medium's fires add to a picture: the sum over its pixels of the channels' rise over
// the same frame with the medium's fires off, in linear light (the codes' curve would weigh a dim
// wide blob above a bright narrow flame).
f64 added_light(const CapturedFrame& on, const CapturedFrame& off) {
  f64 sum = 0.0;
  const u32 n = static_cast<u32>(on.color.size());
  for (u32 i = 0; i < n; i += 4)
    for (u32 c = 0; c < 3; ++c)
      sum += std::max(0.0, linear(on.color[i + c]) - linear(off.color[i + c]));
  return sum;
}

}  // namespace

TEST_CASE("volumetrics: a flame does not vanish or jump as the eye moves away from it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  // R112, the owner's campfire: past R85's 8 m the froxels sampled the flame once a cell, in slices
  // 13% of the distance deep, so walking away from it the flame vanished and came back whole as the
  // slices' centres crossed it. A flame alone (no dust, no smoke) from 9 to 13 m and across the
  // march's hand-over to the froxels' blob (46 to 62 m): its light changes smoothly with distance.
  Rig probe;
  REQUIRE_MESSAGE(probe.build(gpu.device, false, ShadowMode::Off), probe.error);
  FireDesc fire;
  fire.position = probe.data.center + DVec3{0.0, 0.1 * static_cast<f64>(probe.data.radius), 0.0};
  fire.radius_m = 0.3f;
  fire.height_m = 0.8f;
  fire.intensity = 0.12f;  // dim enough that no pixel clips, bright enough to be out of the toe
  fire.flicker = 0.0f;
  fire.smoke_density = 0.0f;
  Rig rig;
  rig.fire = &fire;
  rig.lights = false;
  REQUIRE_MESSAGE(rig.build(gpu.device, false, ShadowMode::Off), rig.error);
  MediumDesc desc;
  desc.dust = false;
  desc.plumes = false;
  MediumOptions without;
  without.fires = false;
  auto light_at = [&](f64 horizontal) {
    const FireView view = look_at_fire(rig.data, fire, horizontal);
    CapturedFrame on, off;
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, {}, view.frame, on), rig.error);
    REQUIRE_MESSAGE(shoot_medium(rig, gpu.device, desc, without, view.frame, off), rig.error);
    // The flame's light falls as the square of the distance; the figure takes that out.
    return added_light(on, off) * view.distance * view.distance;
  };
  for (const f64 start : {9.0, 46.0}) {
    const f64 stride = start < 20.0 ? 0.25 : 1.0;
    f64 lo = 1e30, hi = 0.0, worst = 0.0, last = 0.0;
    for (u32 i = 0; i <= 16; ++i) {
      const f64 v = light_at(start + stride * i);
      MESSAGE("  at " << start + stride * i << " m: " << v);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      if (i > 0 && last > 0.0) worst = std::max(worst, std::abs(v - last) / last);
      last = v;
    }
    MESSAGE("from " << start << " m: the flame's light times d^2 from " << lo << " to " << hi
                    << ", the largest step between neighbours " << worst * 100.0 << "%");
    CHECK(lo > 0.0);  // never gone
    // Near, the flame is ten pixels tall and its light times d^2 holds within 10% (measured 5.4%,
    // a slow rise from 9 to 13 m; the largest step 1.5%).
    // Across the hand-over it is two pixels across, the march's one ray a pixel weighs its core
    // over its edges and the blob does not, and the figure rises smoothly by about 2
    // (measured 1.98) from the march's end to the blob's; what the check holds is no step and no
    // gap.
    CHECK(hi / lo < (start < 20.0 ? 1.1 : 2.5));
    CHECK(worst < 0.25);
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
