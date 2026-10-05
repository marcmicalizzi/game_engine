// The sand's ripples against the game clock (docs/subsystems/renderer.md, "Ripples that move";
// docs/experiments/ripple-motion-2026-10-04.md). At the game's own rate a ripple creeps; until
// 2026-10-04 it held still for seconds and then jumped, because the ground's transport reached the
// renderer in whole cm^2 (`terrain::WindRecord::integral`) and a cm^2 is 20 cm of the ergs'
// ripples' travel. These cases hold the motion to the clock, at the scene's own time (three years
// in, where desert-endless stands) and near the clock's zero:
//
// - **On the CPU**, a fine sweep of clock values at 0.1x, 1x, 60x and 3,600x, a frame a sixtieth of
//   a second at each: the transport is the record's straight line within every hour, and the travel
//   the block hands the shader advances by the transport's step every frame, to two floats of its
//   reduced period, with no step at 1x larger than a hundredth of a wavelength.
// - **On a GPU, the instrument**: the detail view (the ripples' height, unfiltered) drawn at two
//   clock values a known time apart, and the ripples' displacement along the wind between the two
//   pictures read back by correlation — the second picture against the first sampled at every
//   pixel's ground point less a trial displacement — which must be the block's travel between them.
//   And a frame at clock value T is the same bytes reached frame by frame or by a jump.
// - The instrument's long series (a capture a game second for a game minute) is a measurement, run
//   by name, skipped by default.
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/gfx/ground_detail.h>
#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr f64 k_frame_s = 1.0 / 60.0;
constexpr f64 k_hour_s = 3600.0;
constexpr f64 k_day_s = 86'400.0;

bool read_endless(SceneDesc& scene) {
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-endless/scene.json",
                      "content/test-scenes/desert-endless/scene.json");
  if (!test::path_exists(path)) return false;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, scene, error), error);
  return true;
}

// The travel the ground's transport gives at `time_s`, in double: what the block reduces.
f64 travel_at(const TerrainSampler& ground, f64 celerity, f64 time_s) {
  f64 moved = 0.0;
  f32 strength = 0.0f;
  REQUIRE(ground.transport(time_s, moved, strength));
  return moved * celerity;
}

// A step of the block's travel, unwrapped across its reduction.
f64 unwrapped_step(f32 from, f32 to, f64 period) {
  f64 d = static_cast<f64>(to) - static_cast<f64>(from);
  if (d > 0.5 * period) d -= period;
  if (d < -0.5 * period) d += period;
  return d;
}

// The first day from `from_day` on whose wind moves sand: its 06:00, a clock value near zero.
f64 first_windy_morning(const TerrainSampler& ground, f64 celerity, i64 from_day) {
  for (i64 day = from_day; day < from_day + 60; ++day) {
    const f64 start = static_cast<f64>(day) * k_day_s;
    if (travel_at(ground, celerity, start + k_day_s) > travel_at(ground, celerity, start))
      return start + 6.0 * k_hour_s;
  }
  return static_cast<f64>(from_day) * k_day_s + 6.0 * k_hour_s;
}

struct Sweep {
  f64 rate = 0.0;
  f64 start_s = 0.0;
  u32 frames = 0;
  f64 largest_step_m = 0.0;  // the block's largest step of travel in a frame
  f64 step_error_m = 0.0;    // the worst difference of the block's step from the transport's
  f64 line_error_m = 0.0;    // the worst distance of the transport from its hour's straight line
  f64 travel_m = 0.0;        // the block's travel over the sweep
  u32 jumps = 0;             // frames whose step is more than ten times the sweep's mean step
  u32 standing = 0;          // frames whose step is zero while the transport's is not
  f64 first_jump_s = -1.0;   // game seconds into the sweep of the first and last such jump
  f64 last_jump_s = -1.0;
  f64 jump_m = 0.0;  // the largest such jump
};

// `frames` frames at `rate` from `start_s`, each a sixtieth of a second of real time, each block
// made from its two clock values alone as `GpuScene::ground_detail_params` makes it.
Sweep sweep(const TerrainSampler& ground, const gfx::GroundDetailDesc& desc, u32 seed, f64 start_s,
            f64 rate, u32 frames) {
  Sweep out;
  out.rate = rate;
  out.start_s = start_s;
  out.frames = frames;
  const f64 celerity = static_cast<f64>(desc.ripple_celerity);
  const gfx::GroundDetailParams base = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, seed);
  const f64 period = gfx::k_ground_travel_period * static_cast<f64>(base.wavelength);
  const f64 dt = rate * k_frame_s;
  const f64 mean_step = (travel_at(ground, celerity, start_s + dt * static_cast<f64>(frames)) -
                         travel_at(ground, celerity, start_s)) /
                        static_cast<f64>(frames);
  f32 last = 0.0f;
  f64 last_travel = travel_at(ground, celerity, start_s);
  for (u32 f = 0; f <= frames; ++f) {
    const f64 t = start_s + dt * static_cast<f64>(f);
    gfx::GroundDetailParams block = base;
    terrain_detail_motion(ground, desc, t, t - dt, block);
    REQUIRE((block.flags & gfx::k_ground_motion) != 0u);
    // The transport against its hour's straight line: the record is linear within every hour.
    const f64 hour = std::floor(t / k_hour_s) * k_hour_s;
    const f64 a = travel_at(ground, celerity, hour);
    const f64 b = travel_at(ground, celerity, hour + k_hour_s);
    const f64 travel = travel_at(ground, celerity, t);
    out.line_error_m =
        std::max(out.line_error_m, std::abs(travel - (a + (b - a) * (t - hour) / k_hour_s)));
    if (f > 0) {
      const f64 step = unwrapped_step(last, block.travel, period);
      const f64 expected = travel - last_travel;
      out.largest_step_m = std::max(out.largest_step_m, step);
      out.step_error_m = std::max(out.step_error_m, std::abs(step - expected));
      out.travel_m += step;
      if (step == 0.0 && expected > 0.0) ++out.standing;
      if (mean_step > 0.0 && step > 10.0 * mean_step) {
        ++out.jumps;
        const f64 when = t - start_s;
        if (out.first_jump_s < 0.0) out.first_jump_s = when;
        out.last_jump_s = when;
        out.jump_m = std::max(out.jump_m, step);
      }
    }
    last = block.travel;
    last_travel = travel;
  }
  return out;
}

std::string describe(const Sweep& s, f64 wavelength) {
  char line[512];
  const f64 game_s = s.rate * k_frame_s * static_cast<f64>(s.frames);
  std::snprintf(line, sizeof line,
                "x%g from %.3f s, %u frames (%.4g game s): travel %.6g m (%.4g cm a game minute); "
                "largest step %.4g m (%.4g wavelengths); worst step error %.3g m; worst distance "
                "from the hour's line %.3g m; %u frames standing; %u jumps (first %.4g s, last "
                "%.4g s in, the largest %.4g m)",
                s.rate, s.start_s, s.frames, game_s, s.travel_m, s.travel_m / game_s * 6000.0,
                s.largest_step_m, s.largest_step_m / wavelength, s.step_error_m, s.line_error_m,
                s.standing, s.jumps, s.first_jump_s, s.last_jump_s, s.jump_m);
  return line;
}

}  // namespace

TEST_CASE("sand motion: the travel handed to the shader follows the clock at every rate") {
  SceneDesc scene;
  if (!read_endless(scene)) {
    MESSAGE("the desert-endless scene is not here; skipped");
    return;
  }
  const TerrainSampler ground(scene.terrain);
  REQUIRE(ground.ok());
  const gfx::GroundDetailDesc desc = terrain_detail_desc(scene.terrain);
  REQUIRE(desc.ripple_celerity > 0.0f);
  const f64 celerity = static_cast<f64>(desc.ripple_celerity);
  const f64 wavelength = static_cast<f64>(desc.ripple_wavelength);
  const gfx::GroundDetailParams base =
      gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, scene.terrain.seed);
  const f64 period = gfx::k_ground_travel_period * static_cast<f64>(base.wavelength);
  // Two floats of the reduced period: the block's travel is a float under it.
  const f64 float_step = 2.0 * std::ldexp(1.0, std::ilogb(period) - 23);

  // The scene's time, three years in (where desert-endless stands), and a clock value near zero.
  const f64 three_years = scene.terrain.time_s;
  const f64 near_zero = first_windy_morning(ground, celerity, 1);
  const f64 year = travel_at(ground, celerity, three_years + 365.0 * k_day_s) -
                   travel_at(ground, celerity, three_years);
  MESSAGE("the year from the scene's time: the ripples travel " << year / 365.0 << " m a mean day, "
                                                                << year / 365.0 * 100.0 / 1440.0
                                                                << " cm a game minute");
  for (const f64 start : {three_years, near_zero}) {
    const i64 day = static_cast<i64>(std::floor(start / k_day_s));
    const f64 day_travel = travel_at(ground, celerity, static_cast<f64>(day + 1) * k_day_s) -
                           travel_at(ground, celerity, static_cast<f64>(day) * k_day_s);
    MESSAGE("day " << day << ": the ripples travel " << day_travel << " m, "
                   << day_travel * 100.0 / 1440.0 << " cm a game minute on average");
    for (const f64 rate : {0.1, 1.0, 60.0, 3600.0}) {
      const Sweep s = sweep(ground, desc, scene.terrain.seed, start, rate, 3600);
      MESSAGE(describe(s, wavelength));
      // Continuous: the record's line within every hour, to a micrometre of travel.
      CHECK(s.line_error_m < 1e-6);
      // Every frame's step is the transport's step, to the float the shader holds it in.
      CHECK(s.step_error_m <= float_step);
      CHECK(s.standing == 0u);
      // Within an hour (60x sweeps one) the wind's rate is one number, so no frame's step stands
      // out of the rest; over 3,600x's sixty hours a storm's hour may move sixteen times a calm
      // one's, which is the wind and not a step.
      if (rate <= 60.0) {
        CHECK(s.jumps == 0u);
      }
      // And at the game's own rate and below it creeps: no frame's step is a hundredth of a
      // wavelength.
      if (rate <= 1.0) {
        CHECK(s.largest_step_m < 0.01 * wavelength);
      }
    }
  }
}

namespace {

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

  // A frame at the surface's clock value `time_s` whose last frame stood at `previous_s`.
  bool shoot(const Camera& camera, f64 previous_s, f64 time_s, gfx::ResolveMode mode,
             CapturedFrame& out) {
    scene.set_ground_time(previous_s);
    scene.set_ground_time(time_s);
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = 0;
    frame.view_mode = static_cast<u32>(mode);
    CaptureChannels channels;
    channels.depth = true;
    return renderer.render_offscreen(frame, &error) &&
           renderer.capture(frame, channels, out, &error);
  }
};

// The endless desert's ground — its bands, wind, storms, sand and time — as a 64 m grid at 50 cm
// round the origin, which is what a renderer without the world's tiles draws: the ripples are a
// function of position and the clock, not of how the ground is cut.
SceneDesc endless_patch(const SceneDesc& endless, const std::string& ddc) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain = endless.terrain;
  desc.terrain.size = 129;
  desc.terrain.extent = 64.0f;
  desc.ddc = ddc;
  return desc;
}

// A windward slope near the origin with ripples on it: of a 2 m lattice over 40 m square, the point
// whose ground climbs fastest along the wind at under twelve degrees.
Vec3 windward_point(const TerrainSampler& ground, Vec2 wind) {
  Vec3 best{0.0f, ground.height(0.0f, 0.0f), 0.0f};
  f32 most = -1.0f;
  for (i32 j = -10; j <= 10; ++j) {
    for (i32 i = -10; i <= 10; ++i) {
      const f32 x = 2.0f * static_cast<f32>(i);
      const f32 z = 2.0f * static_cast<f32>(j);
      constexpr f32 e = 0.25f;
      const f32 hx = (ground.height(x + e, z) - ground.height(x - e, z)) / (2.0f * e);
      const f32 hz = (ground.height(x, z + e) - ground.height(x, z - e)) / (2.0f * e);
      const f32 climb = hx * wind.x + hz * wind.y;
      if (std::sqrt(hx * hx + hz * hz) > std::tan(radians(12.0f)) || climb <= most) continue;
      most = climb;
      best = Vec3{x, ground.height(x, z), z};
    }
  }
  return best;
}

// A camera 1.5 m over the slope, a metre upwind of the point and looking down at it along the wind.
Camera over(const TerrainSampler& ground, Vec3 point, Vec2 wind) {
  Camera camera;
  const f32 x = point.x - wind.x;
  const f32 z = point.z - wind.y;
  camera.position = absolute(WorldPos::origin(), Vec3{x, ground.height(x, z) + 1.5f, z});
  camera.target = absolute(WorldPos::origin(), point);
  camera.znear = 0.05f;
  return camera;
}

// **The instrument.** The ripples' displacement along the wind from picture `a` to picture `b`,
// both of the detail view (red: the ripples' height, unfiltered) from one camera over a ground that
// does not move: the trial displacement d within half a wavelength of `guess` at which `b` at every
// pixel best matches `a` at that pixel's ground point less d along the wind (projected back into
// `a`, bilinear in its red), by least squares on a grid of a fiftieth of a wavelength refined by a
// parabola. Every pixel's ground point comes from its depth. The ripples' crests move by the
// travel along their own direction, which the steering and the kernels' spread turn by a few
// degrees from the block's wind, so what this reads is the travel times their mean cosine.
struct Instrument {
  Mat4 view_proj;
  Vec3 eye;
  Vector<Vec3> ground;  // the ground point of every other pixel of every other row, from its depth
  Vector<f32> climb;    // the ground's rise there per metre along the wind
  Vector<u32> pixels;   // which pixel each is

  // The pixels' ground points from `shot`'s depth, and the ground's rise along `wind` at each from
  // the height function (a trial displacement moves a point along the ground, not the horizontal).
  // `vp` is the frame's, whose origin is the eye (ADR-0053): `eye_at` is that eye in float32, which
  // takes a point the inverse gives back to the world and a world point into the frame.
  void prepare(const CapturedFrame& shot, const Mat4& vp, Vec3 eye_at,
               const TerrainSampler& terrain, Vec2 wind) {
    view_proj = vp;
    eye = eye_at;
    const Mat4 inv = inverse(vp);
    ground.clear();
    pixels.clear();
    for (u32 py = 4; py + 4 < shot.height; py += 2) {
      for (u32 px = 4; px + 4 < shot.width; px += 2) {
        const u32 p = py * shot.width + px;
        const f32 depth = shot.depth[p];
        if (!(depth > 0.0f)) continue;
        const f32 x = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(shot.width) * 2.0f - 1.0f;
        const f32 y = 1.0f - (static_cast<f32>(py) + 0.5f) / static_cast<f32>(shot.height) * 2.0f;
        const Vec4 w = inv * Vec4{x, y, depth, 1.0f};
        if (!(std::abs(w.w) > 0.0f)) continue;
        const Vec3 g = Vec3{w.x / w.w, w.y / w.w, w.z / w.w} + eye;
        constexpr f32 e = 0.02f;
        climb.push_back((terrain.height(g.x + e * wind.x, g.z + e * wind.y) -
                         terrain.height(g.x - e * wind.x, g.z - e * wind.y)) /
                        (2.0f * e));
        ground.push_back(g);
        pixels.push_back(p);
      }
    }
  }

  // `a`'s red at the screen position of world point `q`; false off its covered pixels.
  bool sample(const CapturedFrame& a, Vec3 q, f64& out) const {
    const Vec3 r = q - eye;
    const Vec4 c = view_proj * Vec4{r.x, r.y, r.z, 1.0f};
    if (!(c.w > 0.0f)) return false;
    const f64 fx = (static_cast<f64>(c.x / c.w) * 0.5 + 0.5) * static_cast<f64>(a.width) - 0.5;
    const f64 fy = (0.5 - static_cast<f64>(c.y / c.w) * 0.5) * static_cast<f64>(a.height) - 0.5;
    if (!(fx >= 0.0 && fy >= 0.0 && fx < static_cast<f64>(a.width - 1) &&
          fy < static_cast<f64>(a.height - 1)))
      return false;
    const u32 x0 = static_cast<u32>(fx);
    const u32 y0 = static_cast<u32>(fy);
    const f64 tx = fx - static_cast<f64>(x0);
    const f64 ty = fy - static_cast<f64>(y0);
    const u32 p = y0 * a.width + x0;
    if (!(a.depth[p] > 0.0f && a.depth[p + 1] > 0.0f && a.depth[p + a.width] > 0.0f &&
          a.depth[p + a.width + 1] > 0.0f))
      return false;
    const auto red = [&](u32 i) { return static_cast<f64>(a.color[i * 4]); };
    const f64 top = red(p) + (red(p + 1) - red(p)) * tx;
    const f64 bottom = red(p + a.width) + (red(p + a.width + 1) - red(p + a.width)) * tx;
    out = top + (bottom - top) * ty;
    return true;
  }

  // The mean squared difference at trial displacement `d`; a large number where too few pixels
  // could be compared.
  f64 misfit(const CapturedFrame& a, const CapturedFrame& b, Vec2 wind, f64 d) const {
    f64 sum = 0.0;
    u32 n = 0;
    for (u32 k = 0; k < pixels.size(); ++k) {
      const Vec3 g = ground[k];
      // The point d back along the wind on the same ground.
      const f32 back = static_cast<f32>(d);
      const f32 qx = g.x - back * wind.x;
      const f32 qz = g.z - back * wind.y;
      const f32 qy = g.y - back * climb[k];
      f64 v = 0.0;
      if (!sample(a, Vec3{qx, qy, qz}, v)) continue;
      const f64 e = static_cast<f64>(b.color[pixels[k] * 4]) - v;
      sum += e * e;
      ++n;
    }
    return n * 2u > pixels.size() ? sum / static_cast<f64>(n) : 1e30;
  }

  f64 displacement(const CapturedFrame& a, const CapturedFrame& b, Vec2 wind, f64 wavelength,
                   f64 guess) const {
    constexpr i32 k_steps = 25;  // either side: a fiftieth of a wavelength a step
    const f64 step = wavelength / (2.0 * k_steps);
    f64 best = guess;
    f64 best_misfit = 1e30;
    i32 best_k = 0;
    f64 misfits[2 * k_steps + 1];
    for (i32 k = -k_steps; k <= k_steps; ++k) {
      const f64 d = guess + step * static_cast<f64>(k);
      misfits[k + k_steps] = misfit(a, b, wind, d);
      if (misfits[k + k_steps] < best_misfit) {
        best_misfit = misfits[k + k_steps];
        best = d;
        best_k = k;
      }
    }
    if (best_k > -k_steps && best_k < k_steps) {
      const f64 m0 = misfits[best_k + k_steps - 1];
      const f64 m1 = misfits[best_k + k_steps];
      const f64 m2 = misfits[best_k + k_steps + 1];
      const f64 curvature = m0 - 2.0 * m1 + m2;
      if (curvature > 0.0) best += 0.5 * step * (m0 - m2) / curvature;
    }
    return best;
  }
};

struct Setup {
  SceneDesc endless;
  SceneDesc patch;
  Vec2 wind;
  Vec3 point;
  Camera camera;
};

}  // namespace

TEST_CASE("sand motion: two frames a known time apart show the ripples moved by the travel") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  Setup s;
  if (!read_endless(s.endless)) {
    MESSAGE("the desert-endless scene is not here; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_motion"};
  s.patch = endless_patch(s.endless, tmp.native().generic_string() + "/ddc");
  // A ripple s times as long moves 1/s as far, and the steering turns a crest off the wind: with
  // neither the spacing, the patches nor the steering every crest moves the travel along the wind.
  s.patch.terrain.detail.spacing_gain = 0.0f;
  s.patch.terrain.detail.patch_size = 0.0f;
  s.patch.terrain.detail.steer_max_deg = 0.0f;
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.sun_azimuth_deg = 200.0f;
  settings.sun_elevation_deg = 18.0f;
  constexpr u32 k_width = 320;
  constexpr u32 k_height = 240;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, s.patch, settings, k_width, k_height), rig.error);
  REQUIRE(rig.scene.ground_detail());
  const TerrainSampler ground(rig.data.terrain);
  const gfx::GroundDetailDesc desc = terrain_detail_desc(rig.data.terrain);
  const f64 celerity = static_cast<f64>(desc.ripple_celerity);
  const f64 wavelength = static_cast<f64>(desc.ripple_wavelength);
  // Three years in, half a minute after the scene's time: the wind there, a windward slope and a
  // camera over it.
  const f64 t1 = rig.data.terrain.time_s + 30.0;
  s.wind = ground.wind(t1);
  s.point = windward_point(ground, s.wind);
  s.camera = over(ground, s.point, s.wind);
  MESSAGE("the camera at (" << s.camera.position.x << ", " << s.camera.position.y << ", "
                            << s.camera.position.z << ") over the slope at (" << s.point.x << ", "
                            << s.point.z << "), the wind (" << s.wind.x << ", " << s.wind.y << ")");
  // The second frame a fifth of a wavelength of travel later at the day's rate, which the
  // instrument reads without ambiguity (the pattern repeats every wavelength along the wind).
  const f64 rate_m_s =
      (travel_at(ground, celerity, t1 + k_hour_s) - travel_at(ground, celerity, t1)) / k_hour_s;
  REQUIRE(rate_m_s > 0.0);
  const f64 dt = std::clamp(0.2 * wavelength / rate_m_s, 0.25, 600.0);
  const f64 t2 = t1 + dt;
  CapturedFrame a;
  CapturedFrame b;
  REQUIRE_MESSAGE(rig.shoot(s.camera, t1 - k_frame_s, t1, gfx::ResolveMode::GroundDetail, a),
                  rig.error);
  REQUIRE_MESSAGE(rig.shoot(s.camera, t2 - k_frame_s, t2, gfx::ResolveMode::GroundDetail, b),
                  rig.error);
  Instrument instrument;
  instrument.prepare(a, rig.renderer.views()[0].view_proj,
                     relative(s.camera.position, WorldPos::origin()), ground, s.wind);
  REQUIRE(instrument.pixels.size() > 1000u);
  const f64 expected = travel_at(ground, celerity, t2) - travel_at(ground, celerity, t1);
  const f64 measured = instrument.displacement(a, b, s.wind, wavelength, expected);
  const f64 still = instrument.displacement(a, a, s.wind, wavelength, 0.0);
  MESSAGE(dt << " game s apart the block's travel moved " << expected * 1000.0
             << " mm and the pictures say " << measured * 1000.0 << " mm (a picture against "
             << "itself: " << still * 1000.0 << " mm)");
  CHECK(std::abs(still) < 0.002 * wavelength);
  // The crests move by the travel along their own directions, the kernels' spread a few degrees off
  // the block's wind, and the picture is 8-bit: within a twentieth.
  CHECK(std::abs(measured - expected) < 0.05 * expected);

  // A frame at clock value T is the same bytes however T was reached: frame by frame for five
  // seconds up to it, or straight there from a day later.
  const f64 t = t2 + 1.0;
  const auto stepping = [&](u32 f) { return t - 5.0 + 5.0 * static_cast<f64>(f) / 300.0; };
  for (u32 f = 0; f < 300; ++f)
    rig.scene.set_ground_time(stepping(f));
  FrameDesc frame;
  frame.camera = s.camera;
  frame.frame_index = 0;
  CaptureChannels channels;
  CapturedFrame stepped;
  rig.scene.set_ground_time(t);
  REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, stepped, &rig.error), rig.error);
  CapturedFrame jumped;
  rig.scene.set_ground_time(t + k_day_s);
  rig.scene.set_ground_time(stepping(299));  // the same last frame, so the same shutter
  rig.scene.set_ground_time(t);
  REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, jumped, &rig.error), rig.error);
  REQUIRE(stepped.color.size() == jumped.color.size());
  CHECK(std::memcmp(stepped.color.data(), jumped.color.data(), stepped.color.size()) == 0);
}

TEST_CASE("sand motion: the instrument's series, a capture a game second for a game minute" *
          doctest::skip()) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  Setup s;
  if (!read_endless(s.endless)) {
    MESSAGE("the desert-endless scene is not here; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_sand_motion_series"};
  s.patch = endless_patch(s.endless, tmp.native().generic_string() + "/ddc");
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  constexpr u32 k_width = 320;
  constexpr u32 k_height = 240;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, s.patch, settings, k_width, k_height), rig.error);
  REQUIRE(rig.scene.ground_detail());
  const TerrainSampler ground(rig.data.terrain);
  const gfx::GroundDetailDesc desc = terrain_detail_desc(rig.data.terrain);
  const f64 celerity = static_cast<f64>(desc.ripple_celerity);
  const f64 wavelength = static_cast<f64>(desc.ripple_wavelength);
  // The clock as the owner's flight had it: from the scene's time, at 1x, a game second a capture,
  // each frame's last frame a sixtieth of a second before it.
  const f64 t0 = rig.data.terrain.time_s;
  s.wind = ground.wind(t0);
  s.point = windward_point(ground, s.wind);
  s.camera = over(ground, s.point, s.wind);
  constexpr u32 k_captures = 91;
  CapturedFrame last;
  REQUIRE_MESSAGE(rig.shoot(s.camera, t0 - k_frame_s, t0, gfx::ResolveMode::GroundDetail, last),
                  rig.error);
  Instrument instrument;
  instrument.prepare(last, rig.renderer.views()[0].view_proj,
                     relative(s.camera.position, WorldPos::origin()), ground, s.wind);
  std::string series =
      "game s since the scene's time; the block's travel since then (mm); the pictures' "
      "displacement along the wind since the last capture and since the first (mm):\n";
  f64 measured_total = 0.0;
  const f64 travel0 = travel_at(ground, celerity, t0);
  for (u32 c = 1; c < k_captures; ++c) {
    const f64 t = t0 + static_cast<f64>(c);
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.shoot(s.camera, t - k_frame_s, t, gfx::ResolveMode::GroundDetail, shot),
                    rig.error);
    // The step between consecutive captures, read within half a wavelength of none: a jump of
    // more than that reads modulo the wavelength.
    const f64 step = instrument.displacement(last, shot, s.wind, wavelength, 0.0);
    measured_total += step;
    char line[128];
    std::snprintf(line, sizeof line, "%u %.3f %.3f %.3f\n", c,
                  (travel_at(ground, celerity, t) - travel0) * 1000.0, step * 1000.0,
                  measured_total * 1000.0);
    series += line;
    last = std::move(shot);
  }
  MESSAGE(series);
}
