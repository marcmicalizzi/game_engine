// A moving terrain on the GPU (docs/subsystems/renderer.md, "The dunes in time-lapse"): the dune
// generator's terrain under a time-lapse is a deformed instance whose pool pass blends two
// evaluated height fields, so these cases hold the pool's output against the CPU's model of it —
// the level's two fields at the motion's times, blended as deform.slang blends them, interpolated
// over the grid's own triangles — at every pixel of every frame, and hold the bound on how far any
// vertex moves in one frame. And the visibility invariants on the pool-fed terrain: occlusion
// culling and the cone test change no pixel (the cone test does not run on a deformed instance at
// all, and a rigid copy of the same terrain shows it would have culled), and the hardware and
// software rasterizers — and the ray path where the device traces — draw the same surface. The
// same again with the terrain rings in the scene (renderer.md, "The rings in the scene"): every
// level draws its own blended field at one surface time, the rings re-centre under a moving camera
// and every frame, the swaps' included, draws the continuous model, and the invariants hold over
// the rings' slots as they do over the scene's grid.
// Compiled only where the terrain capability is; each GPU case skips with a message where there is
// no device.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_rings.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
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

// The rings for a 128 m terrain at a metre: an inner ring 6 m either side at 25 cm and a middle one
// 20 m either side at 50 cm, each a third of the one round it or less, as `validate_rings` wants.
// The tunables are the process's, so they are put back when the case ends.
struct SmallRings {
  SmallRings() {
    set("terrain.rings.inner_half_m", "6");
    set("terrain.rings.inner_spacing_cm", "25");
    set("terrain.rings.middle_half_m", "20");
    set("terrain.rings.middle_spacing_cm", "50");
  }
  ~SmallRings() {
    for (const char* name : {"terrain.rings.inner_half_m", "terrain.rings.inner_spacing_cm",
                             "terrain.rings.middle_half_m", "terrain.rings.middle_spacing_cm"}) {
      if (tunables::Tunable* t = tunables::find(name)) t->reset();
    }
  }
  static void set(const char* name, const char* value) {
    tunables::Tunable* t = tunables::find(name);
    REQUIRE_MESSAGE(t != nullptr, name);
    std::string error;
    REQUIRE_MESSAGE(t->set_from_text(value, &error), error);
  }
};

// Twice the moving scene's side at the same spacing, so two rings fit inside it.
SceneDesc ring_scene(const std::string& ddc) {
  SceneDesc desc = moving_scene(ddc);
  desc.terrain.size = 129;
  desc.terrain.extent = 64.0f;
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  std::unique_ptr<TerrainRingSet> rings;  // outlives the scene and the motion
  GpuScene scene;
  SceneRenderer renderer;
  TerrainMotion motion;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height, const TimeLapseConfig* lapse, jobs::JobSystem* jobs,
             WorldPos ring_camera = WorldPos::origin()) {
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (resolved.terrain_rings) {
      rings = std::make_unique<TerrainRingSet>();
      if (!rings->build(data.terrain, ring_camera, jobs, &error)) return false;
    }
    if (!scene.create(device, data, resolved, &error, rings.get())) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return lapse == nullptr || motion.start(scene, rings.get(), *lapse, jobs, &error);
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

// The world point a covered pixel's depth stands for, through the view's inverse projection. The
// view's matrix is in the frame's space, whose origin is the camera's eye (ADR-0053), so the point
// is put back in the world by adding `eye`: the camera's position, which here is by the origin.
bool unproject(const CapturedFrame& shot, const Mat4& inverse_view_proj, Vec3 eye, u32 px, u32 py,
               Vec3& out) {
  const f32 depth = shot.depth[py * shot.width + px];
  if (!(depth > 0.0f)) return false;
  const f32 x = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(shot.width) * 2.0f - 1.0f;
  const f32 y = 1.0f - (static_cast<f32>(py) + 0.5f) / static_cast<f32>(shot.height) * 2.0f;
  const Vec4 p = inverse_view_proj * Vec4{x, y, depth, 1.0f};
  if (!(std::abs(p.w) > 0.0f)) return false;
  out = Vec3{p.x / p.w, p.y / p.w, p.z / p.w} + eye;
  return true;
}

Camera looking_down() {
  Camera camera;
  camera.position = absolute(WorldPos::origin(), Vec3{3.0f, 38.0f, 30.0f});
  camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, -4.0f});
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
  const f32 eye_y = terrain_height(desc, x, z) + 1.0f;
  camera.position = absolute(WorldPos::origin(), Vec3{x, eye_y, z});
  camera.target = absolute(WorldPos::origin(), Vec3{x + 30.0f * std::cos(0.6f + a), eye_y - 1.5f,
                                                    z - 30.0f * std::sin(0.6f + a)});
  camera.znear = 0.05f;
  return camera;
}

// One level's blended field as the motion draws it this frame: the level's two fields at its
// times over the window it draws, blended in floats as deform.slang blends them, and the squares
// it draws inside and leaves to the level inside it.
struct LevelModel {
  TerrainLattice lattice;
  gfx::TerrainField window;  // `heights` unused
  Vector<f32> h;
  bool bounded = false;  // a ring: only inside `square`
  Vec4 square{};
  bool holed = false;  // leaves `hole` to the next finer level
  Vec4 hole{};
};

void model_level(const SceneData& data, const TerrainMotion& motion, const TerrainRingSet* rings,
                 u32 k, LevelModel& out) {
  const TerrainMotion::LevelStats s = motion.level_stats(k);
  const TerrainSampler sampler(data.terrain);
  out.window = gfx::TerrainField{};
  if (k == 0) {
    out.lattice = terrain_scene_lattice(data.terrain);
    out.window.nx = data.terrain.size;
    out.window.nz = data.terrain.size;
  } else {
    out.lattice = rings->lattice(k);
    out.window = rings->field_window(k, motion.ring_layout());
  }
  const u32 n = out.window.nx * out.window.nz;
  const u32 blocks = terrain_window_blocks(out.window.nx, out.window.nz);
  Vector<f32> a(n);
  Vector<f32> b(n);
  evaluate_terrain_window(sampler, s.time_a, out.lattice, out.window.i0, out.window.j0,
                          out.window.nx, out.window.nz, 0, blocks, std::span<f32>(a.data(), n));
  evaluate_terrain_window(sampler, s.time_b, out.lattice, out.window.i0, out.window.j0,
                          out.window.nx, out.window.nz, 0, blocks, std::span<f32>(b.data(), n));
  const f32 t = static_cast<f32>(s.blend);
  out.h.resize(n);
  for (u32 v = 0; v < n; ++v)
    out.h[v] = s.time_b > s.time_a ? a[v] * (1.0f - t) + b[v] * t : a[v];
  out.bounded = k > 0;
  out.square = k > 0 ? rings->square(k, motion.ring_layout()) : Vec4{};
  out.holed = rings != nullptr && k + 1 < rings->level_count();
  out.hole = out.holed ? rings->square(k + 1, motion.ring_layout()) : Vec4{};
}

// The lowest and highest of the four lattice points round (x, z) on a level: whichever diagonal
// its mesh cut the cell along, the surface there lies between them. False off the level's window.
bool cell_bounds(const LevelModel& m, f32 x, f32 z, f32& lo, f32& hi) {
  const f64 fx = (static_cast<f64>(x) - m.lattice.origin_x) / m.lattice.spacing;
  const f64 fz = (static_cast<f64>(z) - m.lattice.origin_z) / m.lattice.spacing;
  const i64 i = static_cast<i64>(std::floor(fx)) - m.window.i0;
  const i64 j = static_cast<i64>(std::floor(fz)) - m.window.j0;
  if (i < 0 || j < 0 || i + 1 >= i64{m.window.nx} || j + 1 >= i64{m.window.nz}) return false;
  const u32 nx = m.window.nx;
  const f32 c[4] = {m.h[static_cast<u32>(j) * nx + static_cast<u32>(i)],
                    m.h[static_cast<u32>(j) * nx + static_cast<u32>(i) + 1],
                    m.h[static_cast<u32>(j + 1) * nx + static_cast<u32>(i)],
                    m.h[static_cast<u32>(j + 1) * nx + static_cast<u32>(i) + 1]};
  lo = std::min(std::min(c[0], c[1]), std::min(c[2], c[3]));
  hi = std::max(std::max(c[0], c[1]), std::max(c[2], c[3]));
  return true;
}

// Whether (x, z) is `margin` or more inside the square.
bool inside(const Vec4& square, f32 x, f32 z, f32 margin) {
  return x > square.x + margin && x < square.z - margin && z > square.y + margin &&
         z < square.w - margin;
}
// Whether it is `margin` or more away from the square's edges, on either side.
bool clear_of(const Vec4& square, f32 x, f32 z, f32 margin) {
  return inside(square, x, z, margin) || !inside(square, x, z, -margin);
}

// The level that draws (x, z), or ~0u within `margin` of a boundary between two levels, where a
// border, a skirt or the scene grid's collapsed edge may stand.
u32 level_at(const Vector<LevelModel>& levels, f32 x, f32 z, f32 margin) {
  for (u32 k = levels.size(); k-- > 0;) {
    const LevelModel& m = levels[k];
    if (m.bounded && !clear_of(m.square, x, z, margin)) return ~0u;
    if (m.bounded && !inside(m.square, x, z, 0.0f)) continue;
    return k;
  }
  return ~0u;
}

// The largest change of a level's blended field between two frames, over the lattice points both
// frames' windows hold.
f64 level_move(const LevelModel& now, const LevelModel& before) {
  const i32 i0 = std::max(now.window.i0, before.window.i0);
  const i32 j0 = std::max(now.window.j0, before.window.j0);
  const i32 i1 = std::min(now.window.i0 + static_cast<i32>(now.window.nx),
                          before.window.i0 + static_cast<i32>(before.window.nx));
  const i32 j1 = std::min(now.window.j0 + static_cast<i32>(now.window.nz),
                          before.window.j0 + static_cast<i32>(before.window.nz));
  f64 most = 0.0;
  for (i32 j = j0; j < j1; ++j) {
    for (i32 i = i0; i < i1; ++i) {
      const f32 a = now.h[static_cast<u32>(j - now.window.j0) * now.window.nx +
                          static_cast<u32>(i - now.window.i0)];
      const f32 b = before.h[static_cast<u32>(j - before.window.j0) * before.window.nx +
                             static_cast<u32>(i - before.window.i0)];
      most = std::max(most, static_cast<f64>(std::abs(a - b)));
    }
  }
  return most;
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
  // A field of this grid is 17 KB: a 4 KB budget copies each over five frames, and the motion
  // must take none as b before its last piece is in a frame, or the pool reads a half-copied slot.
  rig.scene.set_terrain_upload_budget(4096);
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
    const Vec3 eye = relative(frame.camera.position, WorldPos::origin());
    for (u32 py = 0; py < k_height; ++py) {
      for (u32 px = 0; px < k_width; ++px) {
        Vec3 world;
        if (!unproject(shot, inverse_view_proj, eye, px, py, world)) continue;
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

TEST_CASE("terrain motion: the rate changed while it runs, from a standing start, never steps") {
  // engine-view's `,` and `.` (TerrainMotion::set_rate): a scene drawn with `time_rate_live` has
  // its terrain levels at a rate of zero, and the rate then goes to a day a second, a week, an
  // hour, zero and a week again, fields arriving when the worker has them. Every frame the pool
  // draws the model of the motion's own pair and blend, no vertex moves more than the bound, the
  // surface never goes backwards, and at zero it comes to rest and stays there.
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_terrain_rate"};
  const SceneDesc desc = moving_scene(slashes(tmp.native() / "ddc"));
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.time_rate = 0.0;
  settings.time_rate_live = true;
  TimeLapseConfig lapse = time_lapse_config_from_tunables(0.0);
  lapse.wait = false;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 160;
  constexpr u32 k_height = 96;
  Rig rig;
  // Not before the motion has started.
  CHECK_FALSE(rig.motion.set_rate(60.0));
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, &lapse, &pool),
                  rig.error);
  REQUIRE(rig.resolved.terrain_levels);  // at a rate of zero, because the rate may change
  // Without `time_rate_live` a still terrain is drawn rigid, as it always was.
  {
    ResolvedSettings still;
    RenderSettings plain = settings;
    plain.time_rate_live = false;
    resolve_settings(plain, gpu.device.features(), &rig.data, still);
    CHECK_FALSE(still.terrain_levels);
  }
  CHECK_FALSE(rig.motion.set_rate(-1.0));
  CHECK_FALSE(rig.motion.set_rate(std::nan("")));
  const f64 bound = lapse.fraction * rig.scene.terrain_lattice(0).spacing;
  struct Step {
    u32 frames;
    f64 rate;
  };
  constexpr Step k_steps[] = {{10, 0.0},     {40, 86'400.0}, {40, 604'800.0},
                              {20, 3'600.0}, {50, 0.0},      {30, 604'800.0}};
  CaptureChannels channels;
  channels.depth = true;
  f64 worst_error = 0.0;
  f64 worst_vertex_move = 0.0;
  f64 worst_reported_move = 0.0;
  u64 compared = 0;
  bool monotonic = true;
  bool rested = true;
  f64 last_surface = rig.motion.level_stats(0).surface_s;
  Vector<f32> previous;
  Vector<f32> expected;
  u32 f = 0;
  u32 changes = 0;
  for (const Step& step : k_steps) {
    if (step.rate != rig.motion.config().rate) ++changes;
    REQUIRE(rig.motion.set_rate(step.rate));
    CHECK(rig.motion.config().rate == step.rate);
    for (u32 i = 0; i < step.frames; ++i, ++f) {
      rig.motion.frame(1.0 / 60.0);
      worst_reported_move = std::max(worst_reported_move, rig.motion.last_move_m());
      const TerrainMotion::LevelStats s = rig.motion.level_stats(0);
      monotonic = monotonic && s.surface_s >= last_surface;
      // At zero, after the deceleration: at rest.
      if (step.rate == 0.0 && i >= 48) rested = rested && s.surface_s == last_surface;
      last_surface = s.surface_s;
      FrameDesc frame;
      frame.camera = looking_down();
      frame.frame_index = f;
      frame.lod_px = 0.0f;
      CapturedFrame shot;
      REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
      expected_heights(rig.data, s, expected);
      if (!previous.empty()) {
        for (u32 v = 0; v < expected.size(); ++v)
          worst_vertex_move =
              std::max(worst_vertex_move, static_cast<f64>(std::abs(expected[v] - previous[v])));
      }
      previous = expected;
      const Mat4 inverse_view_proj = inverse(rig.renderer.views()[0].view_proj);
      const Vec3 eye = relative(frame.camera.position, WorldPos::origin());
      for (u32 py = 0; py < k_height; py += 2) {
        for (u32 px = 0; px < k_width; px += 2) {
          Vec3 world;
          if (!unproject(shot, inverse_view_proj, eye, px, py, world)) continue;
          const f32 model = surface_at(rig.data.terrain, expected, world.x, world.z);
          if (std::isnan(model)) continue;
          worst_error = std::max(worst_error, static_cast<f64>(std::abs(world.y - model)));
          ++compared;
        }
      }
    }
  }
  rig.motion.finish();
  const TerrainMotion::LevelStats s = rig.motion.level_stats(0);
  MESSAGE(f << " frames over " << changes << " changes of rate: " << s.installed
            << " fields installed, " << s.held << " frames held, " << s.capped
            << " capped; largest vertex move a frame " << worst_vertex_move << " m (bound " << bound
            << "); the pool against the model at " << compared << " pixels, worst " << worst_error
            << " m");
  CHECK(rig.motion.rate_changes() == changes);
  CHECK(rig.motion.start_rate() == 0.0);
  CHECK(s.installed > 2u);
  CHECK(monotonic);
  CHECK(rested);
  CHECK(worst_vertex_move <= bound + 1.0e-5);
  CHECK(worst_reported_move <= bound + 1.0e-9);
  CHECK(compared > 1'000u);
  CHECK(worst_error <= 2.0e-3);
}

namespace {

// The visibility invariants on a moving terrain, over the scene's grid alone or with the rings in
// the scene beside it (re-centred under the camera as it moves along the sand).
void culling_case(bool with_rings) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  std::unique_ptr<SmallRings> small;
  if (with_rings) small = std::make_unique<SmallRings>();
  test::TempDir tmp{"engine_renderer_terrain_motion_culling"};
  const SceneDesc desc = with_rings ? ring_scene(slashes(tmp.native() / "ddc"))
                                    : moving_scene(slashes(tmp.native() / "ddc"));
  RenderSettings base;
  base.shadows = ShadowMode::Off;
  base.time_rate = 604'800.0;
  base.terrain_rings = with_rings;
  const WorldPos ring_camera = looking_across(desc.terrain, 0).position;
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
    if (!rig.build(gpu.device, desc, variants[k].settings, k_width, k_height, &lapse, &pool,
                   ring_camera)) {
      if (variants[k].needs_rays && rig.error == "unavailable") {
        MESSAGE("no ray path here: " << unavailable_reason(
                    RenderAvailability::NoAccelerationStructures, gpu.device));
        continue;
      }
      FAIL("variant " << variants[k].name << ": " << rig.error);
    }
    REQUIRE(rig.resolved.terrain_levels);
    REQUIRE(rig.resolved.terrain_rings == with_rings);
    u32 shot = 0;
    for (u32 f = 0; f < k_frames; ++f) {
      const Camera camera = looking_across(desc.terrain, f);
      rig.motion.frame(1.0 / 60.0, camera.position);
      FrameDesc frame;
      frame.camera = camera;
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
    if (with_rings) {
      const TerrainMotion::RingStats& r = rig.motion.ring_stats();
      MESSAGE("variant " << std::string(variants[k].name) << ": " << r.swaps << " ring re-centres, "
                         << r.chunks_built << " chunks built, " << r.chunks_kept << " kept");
      CHECK(r.failed == 0u);
      CHECK(r.swaps > 0u);
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
  if (with_rings) return;
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

}  // namespace

TEST_CASE("terrain motion: culling changes nothing and the rasterizers agree on a moving terrain") {
  culling_case(false);
}

TEST_CASE("terrain rings: culling changes nothing and the rasterizers agree over the rings") {
  culling_case(true);
}

TEST_CASE(
    "terrain rings: every level draws its blended field at one time, and no re-centre steps") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SmallRings small;
  test::TempDir tmp{"engine_renderer_terrain_rings"};
  const SceneDesc desc = ring_scene(slashes(tmp.native() / "ddc"));
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.time_rate = 604'800.0;
  settings.terrain_rings = true;
  TimeLapseConfig lapse = time_lapse_config_from_tunables(settings.time_rate);
  lapse.wait = true;  // every re-centre is swapped in the frame that asks for it
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 256;
  constexpr u32 k_height = 192;
  constexpr u32 k_frames = 60;
  // 40 m over the sand, looking down steeply enough that the ground under the camera is in view,
  // and sliding along x, 40 cm a frame: the rings follow the camera's position, so the inner ring
  // (6 m either side) is the ground below it and re-centres every few metres, and the middle one
  // (20 m) round it once or twice, with the grid beyond.
  const auto camera_at = [](u32 f) {
    Camera camera;
    const f32 x = -10.0f + 0.4f * static_cast<f32>(f);
    camera.position = absolute(WorldPos::origin(), Vec3{x, 40.0f, 4.0f});
    camera.target = absolute(WorldPos::origin(), Vec3{x + 2.0f, 0.0f, -12.0f});
    camera.znear = 0.5f;
    return camera;
  };
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, &lapse, &pool,
                            camera_at(0).position),
                  rig.error);
  REQUIRE(rig.resolved.terrain_rings);
  REQUIRE(rig.scene.terrain_level_count() == 3u);
  const u32 levels = rig.scene.terrain_level_count();
  CaptureChannels channels;
  channels.depth = true;
  channels.ids = true;
  f64 worst_error = 0.0;
  f64 worst_level_error[3] = {};
  Vec3 worst_at[3] = {};
  u32 worst_frame[3] = {};
  u32 worst_drawn_by[3] = {};
  Vec2 worst_bounds[3] = {};
  u64 compared[3] = {};
  u64 wrong_level[3] = {};
  f64 worst_move[3] = {};
  Vector<LevelModel> models(levels);
  Vector<LevelModel> before;
  const u32 scene_instances = rig.data.instances.size();
  for (u32 f = 0; f < k_frames; ++f) {
    const Camera camera = camera_at(f);
    rig.motion.frame(1.0 / 60.0, camera.position);
    // One surface time: where two levels meet they draw the same sand.
    for (u32 k = 1; k < levels; ++k)
      CHECK(rig.motion.level_stats(k).surface_s == rig.motion.level_stats(0).surface_s);
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = f;
    frame.lod_px = 0.0f;  // the finest clusters: every level's own lattice
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
    for (u32 k = 0; k < levels; ++k)
      model_level(rig.data, rig.motion, rig.rings.get(), k, models[k]);
    // The per-frame bound on every level, a re-centre's frame included: its lattice points move by
    // no more than a fraction of its own spacing.
    if (!before.empty()) {
      for (u32 k = 0; k < levels; ++k)
        worst_move[k] = std::max(worst_move[k], level_move(models[k], before[k]));
    }
    before = models;
    // And every covered pixel against the level that draws it.
    const Mat4 inverse_view_proj = inverse(rig.renderer.views()[0].view_proj);
    const Vec3 eye = relative(frame.camera.position, WorldPos::origin());
    for (u32 py = 0; py < k_height; ++py) {
      for (u32 px = 0; px < k_width; ++px) {
        Vec3 world;
        if (!unproject(shot, inverse_view_proj, eye, px, py, world)) continue;
        const u32 k = level_at(models, world.x, world.z, 2.0f);
        if (k == ~0u) continue;
        f32 lo = 0.0f;
        f32 hi = 0.0f;
        if (!cell_bounds(models[k], world.x, world.z, lo, hi)) continue;
        const f64 error =
            std::max({0.0, static_cast<f64>(lo - world.y), static_cast<f64>(world.y - hi)});
        worst_error = std::max(worst_error, error);
        // Drawn by the level it stands in: the scene grid's own instance outside the rings, a
        // ring's slot inside them — the middle ring's slots first, then the inner one's.
        const u32 instance = shot.ids[(py * k_width + px) * k_id_words];
        const bool ring_slot = instance >= scene_instances;
        const u32 drawn_by =
            !ring_slot ? 0u : (instance - scene_instances < rig.scene.terrain_slots(1) ? 1u : 2u);
        if (error > worst_level_error[k]) {
          worst_level_error[k] = error;
          worst_at[k] = world;
          worst_frame[k] = f;
          worst_drawn_by[k] = drawn_by;
          worst_bounds[k] = Vec2{lo, hi};
        }
        ++compared[k];
        wrong_level[k] += drawn_by != k ? 1u : 0u;
      }
    }
  }
  rig.motion.finish();
  const TerrainMotion::RingStats& r = rig.motion.ring_stats();
  const TerrainMotion::LevelStats s0 = rig.motion.level_stats(0);
  MESSAGE(k_frames << " frames: " << r.swaps << " re-centres swapped (" << r.chunks_built
                   << " chunks built, " << r.chunks_kept << " kept, " << r.upload_bytes
                   << " bytes uploaded, last rebuild " << r.last_rebuild_ms
                   << " ms, arenas at most " << r.arena_peak_share << " full); " << s0.installed
                   << " grid fields installed; pixels compared per level " << compared[0] << ", "
                   << compared[1] << ", " << compared[2] << ", worst distance from the model "
                   << worst_error << " m; largest move a "
                   << "frame " << worst_move[0] << ", " << worst_move[1] << ", " << worst_move[2]
                   << " m");
  CHECK(r.failed == 0u);
  CHECK(r.swaps >= 2u);
  CHECK(r.rebuilds == r.swaps);
  CHECK(s0.installed > 2u);
  for (u32 k = 0; k < levels; ++k) {
    CAPTURE(k);
    MESSAGE("level " << k << ": " << compared[k] << " pixels, " << wrong_level[k]
                     << " drawn by the other kind of level, worst " << worst_level_error[k]
                     << " m at (" << worst_at[k].x << ", " << worst_at[k].y << ", " << worst_at[k].z
                     << ") in frame " << worst_frame[k] << ", drawn by level " << worst_drawn_by[k]
                     << ", the cell between " << worst_bounds[k].x << " and " << worst_bounds[k].y);
    CHECK(compared[k] > 1'000u);
    CHECK(wrong_level[k] == 0u);
    CHECK(worst_level_error[k] <= 5.0e-3);
    CHECK(worst_move[k] <= lapse.fraction * rig.scene.terrain_lattice(k).spacing + 1.0e-5);
  }
  CHECK(worst_error <= 5.0e-3);
}
