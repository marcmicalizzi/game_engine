// A moving terrain on the GPU (docs/subsystems/renderer.md, "The dunes in time-lapse"): the dune
// generator's terrain under a time-lapse is a deformed instance whose pool pass blends two
// evaluated height fields, so these cases hold the pool's output against the CPU's model of it —
// the level's two fields at the motion's times, blended as deform.slang blends them, interpolated
// over the grid's own triangles — at every pixel of every frame, and hold the bound on how far any
// vertex moves in one frame. And the visibility invariants on the pool-fed terrain: occlusion
// culling and the cone test change no pixel (the cone test does not run on a deformed instance at
// all, and a rigid copy of the same terrain shows it would have culled), and the hardware and
// software rasterizers — and the ray path where the device traces — draw the same surface.
// Compiled only where the terrain capability is; each GPU case skips with a message where there is
// no device.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
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

// 64 m of the default bands at a 20 m wavelength, a metre a sample, three years in: enough dunes
// in view that every frame has slopes, crests and floors, and small enough to evaluate in
// milliseconds, so a time-lapse at a week a second installs a field every few frames.
SceneDesc moving_scene(const std::string& ddc) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 65;
  desc.terrain.extent = 32.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0;
  desc.ddc = ddc;
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  TerrainMotion motion;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height, const TimeLapseConfig* lapse, jobs::JobSystem* jobs) {
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
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return lapse == nullptr || motion.start(scene, *lapse, jobs, &error);
  }
};

// What the pool pass draws for level 0, vertex by vertex: the two fields at the motion's times,
// blended in floats as deform.slang blends them (`a (1 - t) + b t`).
void expected_heights(const SceneData& data, const TerrainMotion::LevelStats& s, Vector<f32>& out) {
  const TerrainDesc& desc = data.terrain;
  const TerrainSampler sampler(desc);
  const TerrainLattice lattice = terrain_scene_lattice(desc);
  const u32 n = desc.size;
  const u32 blocks = terrain_window_blocks(n, n);
  Vector<f32> a(n * n);
  Vector<f32> b(n * n);
  evaluate_terrain_window(sampler, s.time_a, lattice, 0, 0, n, n, 0, blocks,
                          std::span<f32>(a.data(), a.size()));
  evaluate_terrain_window(sampler, s.time_b, lattice, 0, 0, n, n, 0, blocks,
                          std::span<f32>(b.data(), b.size()));
  const f32 t = static_cast<f32>(s.blend);
  out.resize(n * n);
  for (u32 v = 0; v < n * n; ++v)
    out[v] = s.time_b > s.time_a ? a[v] * (1.0f - t) + b[v] * t : a[v];
}

// The surface over the grid's own triangles at (x, z): two counter-clockwise triangles a cell, as
// `build_terrain_mesh` makes them. NaN off the grid.
f32 surface_at(const TerrainDesc& desc, const Vector<f32>& h, f32 x, f32 z) {
  const u32 n = desc.size;
  const f32 s = 2.0f * desc.extent / static_cast<f32>(n - 1);
  const f32 fx = (x + desc.extent) / s;
  const f32 fz = (z + desc.extent) / s;
  if (!(fx >= 0.0f) || !(fz >= 0.0f) || fx >= static_cast<f32>(n - 1) ||
      fz >= static_cast<f32>(n - 1)) {
    return std::nanf("");
  }
  const u32 i = static_cast<u32>(fx);
  const u32 j = static_cast<u32>(fz);
  const f32 u = fx - static_cast<f32>(i);
  const f32 v = fz - static_cast<f32>(j);
  const f32 h00 = h[j * n + i];
  const f32 h10 = h[j * n + i + 1];
  const f32 h01 = h[(j + 1) * n + i];
  const f32 h11 = h[(j + 1) * n + i + 1];
  if (u + v <= 1.0f) return h00 + u * (h10 - h00) + v * (h01 - h00);
  return h11 + (1.0f - u) * (h01 - h11) + (1.0f - v) * (h10 - h11);
}

// The world point a covered pixel's depth stands for, through the view's inverse projection.
bool unproject(const CapturedFrame& shot, const Mat4& inverse_view_proj, u32 px, u32 py,
               Vec3& out) {
  const f32 depth = shot.depth[py * shot.width + px];
  if (!(depth > 0.0f)) return false;
  const f32 x = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(shot.width) * 2.0f - 1.0f;
  const f32 y = 1.0f - (static_cast<f32>(py) + 0.5f) / static_cast<f32>(shot.height) * 2.0f;
  const Vec4 p = inverse_view_proj * Vec4{x, y, depth, 1.0f};
  if (!(std::abs(p.w) > 0.0f)) return false;
  out = Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
  return true;
}

Camera looking_down() {
  Camera camera;
  camera.position = Vec3{3.0f, 38.0f, 30.0f};
  camera.target = Vec3{0.0f, 0.0f, -4.0f};
  camera.znear = 0.5f;
  return camera;
}

Camera looking_across(const TerrainDesc& desc, u32 f) {
  // A metre over the sand and looking along it, turning a little each frame, so the cut, the
  // occlusion history and the slopes facing away from the eye all change as the sand does — and a
  // lee face beyond a crest faces away from the eye, which is what the cone test would cull.
  Camera camera;
  const f32 a = 0.02f * static_cast<f32>(f);
  const f32 x = -24.0f + 0.2f * static_cast<f32>(f);
  const f32 z = 20.0f;
  camera.position = Vec3{x, terrain_height(desc, x, z) + 1.0f, z};
  camera.target = Vec3{x + 30.0f * std::cos(0.6f + a), camera.position.y - 1.5f,
                       z - 30.0f * std::sin(0.6f + a)};
  camera.znear = 0.05f;
  return camera;
}

}  // namespace

TEST_CASE("terrain motion: the pool draws the blended field, and no vertex jumps") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_terrain_motion"};
  const SceneDesc desc = moving_scene(slashes(tmp.native() / "ddc"));
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.time_rate = 604'800.0;  // a game week a real second
  TimeLapseConfig lapse;
  lapse.rate = settings.time_rate;
  lapse.fraction = 0.25;
  lapse.min_step_s = 60.0;
  lapse.max_step_s = 30.0 * 86'400.0;
  lapse.lead = 1.5;
  lapse.wait = false;  // fields arrive when the worker has them, late or not
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 192;
  constexpr u32 k_height = 128;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, &lapse, &pool),
                  rig.error);
  REQUIRE(rig.resolved.terrain_levels);
  REQUIRE(rig.resolved.deform_pass);
  REQUIRE(rig.scene.terrain_level_count() == 1u);
  const f64 bound = lapse.fraction * rig.scene.terrain_lattice(0).spacing;
  CaptureChannels channels;
  channels.depth = true;
  channels.ids = true;
  constexpr u32 k_frames = 150;
  f64 worst_error = 0.0;
  f64 worst_vertex_move = 0.0;
  f64 worst_reported_move = 0.0;
  u64 compared = 0;
  Vector<f32> previous;
  Vector<f32> first;
  Vector<f32> expected;
  for (u32 f = 0; f < k_frames; ++f) {
    rig.motion.frame(1.0 / 60.0);
    worst_reported_move = std::max(worst_reported_move, rig.motion.last_move_m());
    FrameDesc frame;
    frame.camera = looking_down();
    frame.frame_index = f;
    frame.lod_px = 0.0f;  // the finest clusters: the grid's own triangles
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
    const TerrainMotion::LevelStats s = rig.motion.level_stats(0);
    expected_heights(rig.data, s, expected);
    if (f == 0) first = expected;
    // The per-frame bound, on every vertex of the grid: the heights the pool pass writes.
    if (!previous.empty()) {
      for (u32 v = 0; v < expected.size(); ++v)
        worst_vertex_move =
            std::max(worst_vertex_move, static_cast<f64>(std::abs(expected[v] - previous[v])));
    }
    previous = expected;
    // And the pool's output against the model, at every covered pixel over the grid.
    const Mat4 inverse_view_proj = inverse(rig.renderer.views()[0].view_proj);
    for (u32 py = 0; py < k_height; ++py) {
      for (u32 px = 0; px < k_width; ++px) {
        Vec3 world;
        if (!unproject(shot, inverse_view_proj, px, py, world)) continue;
        const f32 model = surface_at(rig.data.terrain, expected, world.x, world.z);
        if (std::isnan(model)) continue;
        worst_error = std::max(worst_error, static_cast<f64>(std::abs(world.y - model)));
        ++compared;
      }
    }
  }
  rig.motion.finish();
  const TerrainMotion::LevelStats s = rig.motion.level_stats(0);
  f64 total_move = 0.0;
  for (u32 v = 0; v < expected.size(); ++v)
    total_move = std::max(total_move, static_cast<f64>(std::abs(expected[v] - first[v])));
  MESSAGE(k_frames << " frames at a game week a second: " << s.installed << " fields installed, "
                   << s.held << " frames held, " << s.capped << " capped, " << s.late
                   << " fields timed late; largest vertex move a frame " << worst_vertex_move
                   << " m (bound " << bound << "), largest over the run " << total_move
                   << " m; the pool against the model at " << compared << " pixels, worst "
                   << worst_error << " m");
  CHECK(compared > 10'000u);
  CHECK(s.installed > 2u);
  CHECK(total_move > 4.0 * bound);  // the sand did move, by many frames' worth
  CHECK(worst_vertex_move <= bound + 1.0e-5);
  CHECK(worst_reported_move <= bound + 1.0e-9);
  CHECK(worst_error <= 2.0e-3);
}

TEST_CASE("terrain motion: culling changes nothing and the rasterizers agree on a moving terrain") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_terrain_motion_culling"};
  const SceneDesc desc = moving_scene(slashes(tmp.native() / "ddc"));
  RenderSettings base;
  base.shadows = ShadowMode::Off;
  base.time_rate = 604'800.0;
  TimeLapseConfig lapse = time_lapse_config_from_tunables(base.time_rate);
  lapse.wait = true;  // every rig draws the same sand on the same frame
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 120;
  struct Variant {
    const char* name;
    RenderSettings settings;
    bool needs_rays = false;
  };
  Variant variants[5];
  variants[0] = {"hw, occlusion and cones on", base};
  variants[1] = {"hw, occlusion off", base};
  variants[1].settings.occlusion = false;
  variants[2] = {"hw, cones off", base};
  variants[2].settings.cone = false;
  variants[3] = {"sw", base};
  variants[3].settings.raster = RasterMode::Software;
  variants[4] = {"rt", base, true};
  variants[4].settings.raster = RasterMode::RayTrace;
  constexpr u32 k_frames = 40;
  const u32 shots_at[3] = {9, 24, 39};
  CapturedFrame shots[5][3];
  u32 pairs[5][3] = {};
  bool ran[5] = {};
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  for (u32 k = 0; k < 5; ++k) {
    Rig rig;
    if (!rig.build(gpu.device, desc, variants[k].settings, k_width, k_height, &lapse, &pool)) {
      if (variants[k].needs_rays && rig.error == "unavailable") {
        MESSAGE("no ray path here: " << unavailable_reason(
                    RenderAvailability::NoAccelerationStructures, gpu.device));
        continue;
      }
      FAIL("variant " << variants[k].name << ": " << rig.error);
    }
    REQUIRE(rig.resolved.terrain_levels);
    u32 shot = 0;
    for (u32 f = 0; f < k_frames; ++f) {
      rig.motion.frame(1.0 / 60.0);
      FrameDesc frame;
      frame.camera = looking_across(desc.terrain, f);
      frame.frame_index = f;
      frame.lod_px = 0.5f;
      if (shot < 3 && f == shots_at[shot]) {
        REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shots[k][shot], &rig.error),
                        rig.error);
        pairs[k][shot] = rig.renderer.stats().visible_pairs();
        ++shot;
      } else {
        REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
      }
    }
    rig.motion.finish();
    ran[k] = true;
  }
  struct Difference {
    u64 coverage = 0;       // pixels one covers and the other does not
    u64 ids = 0;            // pixels both cover whose (instance, cluster, triangle) differ
    u64 depths = 0;         // pixels both cover whose depth differs at all
    f64 depth_delta = 0.0;  // the mean |depth difference| over the pixels both cover
    u64 both = 0;
  };
  const auto differ = [&](u32 a, u32 b, u32 s) {
    Difference d;
    const CapturedFrame& x = shots[a][s];
    const CapturedFrame& y = shots[b][s];
    for (u32 p = 0; p < k_width * k_height; ++p) {
      const bool cx = x.depth[p] > 0.0f;
      const bool cy = y.depth[p] > 0.0f;
      d.coverage += cx != cy ? 1u : 0u;
      if (!cx || !cy) continue;
      ++d.both;
      bool same = true;
      for (u32 w = 0; w < k_id_words; ++w)
        same = same && x.ids[p * k_id_words + w] == y.ids[p * k_id_words + w];
      d.ids += same ? 0u : 1u;
      d.depths += x.depth[p] != y.depth[p] ? 1u : 0u;
      d.depth_delta += std::abs(static_cast<f64>(x.depth[p]) - static_cast<f64>(y.depth[p]));
    }
    if (d.both > 0) d.depth_delta /= static_cast<f64>(d.both);
    return d;
  };
  for (u32 s = 0; s < 3; ++s) {
    CAPTURE(shots_at[s]);
    // Occlusion culling changes no pixel: the same words and the same depths.
    Difference d = differ(0, 1, s);
    CHECK(d.coverage == 0u);
    CHECK(d.ids == 0u);
    CHECK(d.depths == 0u);
    // The cone test does not run on a deformed instance: the same pixels and the same pairs.
    d = differ(0, 2, s);
    CHECK(d.coverage == 0u);
    CHECK(d.ids == 0u);
    CHECK(d.depths == 0u);
    CHECK(pairs[0][s] == pairs[2][s]);
    // The software rasterizer and the hardware one read the same pool and agree as they do on a
    // rigid mesh (domain/gfx's visibility test): coverage but for edge rules, the same triangle
    // but for shared edges, depth to a fraction of its precision.
    d = differ(0, 3, s);
    MESSAGE("frame " << shots_at[s] << ": hw against sw differ on " << d.coverage << " covered and "
                     << d.ids << " ids of " << d.both
                     << " pixels both cover, mean depth difference " << d.depth_delta);
    CHECK(d.coverage * 100u < shots[0][s].covered);
    CHECK(d.ids * 100u <= d.both * 10u);
    CHECK(d.depth_delta < 1.0e-3);
    if (ran[4]) {
      // The ray path traces the frame's cut from the pool: the same coverage, and the same
      // surfaces up to what a silhouette costs a rigid mesh too (renderer.md, "Ray tracing a
      // moving character").
      d = differ(0, 4, s);
      MESSAGE("frame " << shots_at[s] << ": hw against rt differ on " << d.coverage
                       << " covered and " << d.ids << " ids of " << d.both);
      CHECK(d.coverage <= shots[0][s].covered / 1000u + 2u);
      CHECK(d.ids <= d.both / 200u + 4u);
    }
  }
  // The case is worth something only if the cone test would cull here: a rigid copy of the same
  // terrain, cones on and off, draws fewer pairs with them.
  RenderSettings rigid = base;
  rigid.time_rate = 0.0;
  RenderSettings rigid_off = rigid;
  rigid_off.cone = false;
  Rig with;
  Rig without;
  REQUIRE_MESSAGE(with.build(gpu.device, desc, rigid, k_width, k_height, nullptr, nullptr),
                  with.error);
  REQUIRE_MESSAGE(without.build(gpu.device, desc, rigid_off, k_width, k_height, nullptr, nullptr),
                  without.error);
  REQUIRE_FALSE(with.resolved.terrain_levels);
  FrameDesc frame;
  frame.camera = looking_across(desc.terrain, 24);
  frame.frame_index = 24;
  frame.lod_px = 0.5f;
  REQUIRE_MESSAGE(with.renderer.render_offscreen(frame, &with.error), with.error);
  REQUIRE_MESSAGE(without.renderer.render_offscreen(frame, &without.error), without.error);
  REQUIRE_MESSAGE(with.renderer.render_offscreen(frame, &with.error), with.error);
  REQUIRE_MESSAGE(without.renderer.render_offscreen(frame, &without.error), without.error);
  MESSAGE("a rigid copy draws " << with.renderer.stats().visible_pairs() << " pairs with cones and "
                                << without.renderer.stats().visible_pairs() << " without");
  CHECK(with.renderer.stats().visible_pairs() < without.renderer.stats().visible_pairs());
}
