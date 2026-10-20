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
    rig.renderer.sky().evaluate(rig.scene.ground_time_s() + mid, sky);
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
  frame.sun_time_s = time_s;
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
  rig.renderer.sky().evaluate(rig.scene.ground_time_s() + night_s, night_sky);
  const Vec3 moon = night_sky.state.moon;
  Picture night;
  REQUIRE_MESSAGE(
      picture(gpu.device, rig, towards(night_s, Vec3{moon.x, 0.2f, moon.z}), "night", tmp, night),
      rig.error);
  // Sunset: the sun 2 degrees up, looking towards it; and dusk, 2 degrees under, the same way.
  const auto low_sun = [&](f32 elevation_deg, const char* name, Picture& out) {
    const f64 time_s = afternoon_at(rig, elevation_deg);
    FrameSky sky;
    rig.renderer.sky().evaluate(rig.scene.ground_time_s() + time_s, sky);
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
