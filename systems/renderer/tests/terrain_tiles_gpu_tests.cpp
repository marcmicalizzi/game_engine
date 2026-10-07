// The ground from the world's tiles, on a GPU (docs/subsystems/renderer.md, "The ground from the
// world's tiles"; ADR-0050), held by the visibility buffer and not by eye:
//
//   - the erg drawn from tiles at the grid's spacing is the fixed grid's picture inside the grid's
//     extent, pixel for pixel, at the finest cut — colour to the bit, depth within a few float
//     steps (a tile is placed at its corner and the grid at cell zero) — and has ground past it;
//   - tiles meet with no crack: every pixel of a view that is all ground is covered, no column of
//     a grazing view has a hole under its ground, and every covered pixel stands on the surface —
//     at one level, across levels, a time-lapse step apart, and 50 km from the origin;
//   - no lighting seam: the shading normal steps across a tile's border no more than it does
//     between two pixels inside a tile;
//   - the time-lapse holds across tiles: one surface time, each level within its bound;
//   - a tile set built ahead and read back from a derived-data cache draws the procedural one's
//     picture, pixel for pixel: the renderer does not know who made a tile;
//   - a long flight keeps the resident tiles bounded and never drops a rebuild;
//   - the translation suite's terrain: tiles and a ground of the test's own moved by whole cells
//     with the camera draw the same ids, depth and normals on every rasterizer and the ray path.
//
// Compiled only where the terrain capability is; each case skips with a message where there is no
// device.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <core/memory/memory.h>
#include <domain/gfx/device.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
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

// The default bands at a 20 m wavelength, 2 m high, three years in: dunes in every view, cheap to
// evaluate. A grid of a few samples: under tiles it only carries the material and the UV frame.
SceneDesc dune_scene(const std::string& ddc, u32 size = 17, f32 extent = 64.0f) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = size;
  desc.terrain.extent = extent;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0;
  desc.ddc = ddc;
  return desc;
}

// 8 m tiles in rings of 1.5, 3 and 6 tiles at 50 cm, 1 m and 2 m: three levels in any view of the
// ground a few tens of metres across.
TerrainTilesDesc small_tiles() {
  TerrainTilesDesc t;
  t.tile_size = 8.0f;
  t.ring_count = 3;
  t.radius[0] = 1.5f;
  t.radius[1] = 3.0f;
  t.radius[2] = 6.0f;
  t.cells[0] = 16;
  t.cells[1] = 8;
  t.cells[2] = 4;
  return t;
}

struct TileRig {
  SceneData data;
  ResolvedSettings resolved;
  std::unique_ptr<TerrainSampler> ground;  // the procedural source's provider
  std::unique_ptr<TerrainTileSet> tiles;   // outlives the scene and the motion
  GpuScene scene;
  SceneRenderer renderer;
  TerrainMotion motion;
  std::string error;

  // With `tile_desc` null, the fixed grid as terrain levels (the reference); otherwise the tiles.
  bool build(const gfx::Device& device, const SceneDesc& desc, RenderSettings settings, u32 width,
             u32 height, const TimeLapseConfig& lapse, jobs::JobSystem* jobs,
             const TerrainTilesDesc* tile_desc, WorldPos camera,
             const scene_gen::TileSource* source = nullptr) {
    if (!load_scene(desc, data, error)) return false;
    settings.terrain_tiles = tile_desc != nullptr;
    settings.time_rate_live = true;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (tile_desc != nullptr) {
      if (!resolved.terrain_tiles) {
        error = "the settings did not resolve to world tiles";
        return false;
      }
      ground = std::make_unique<TerrainSampler>(data.terrain);
      tiles = std::make_unique<TerrainTileSet>();
      const scene_gen::TileSource src = source != nullptr ? *source : ground->provider().tiles();
      if (!tiles->build(data.terrain, *tile_desc, src, camera, jobs, &error)) return false;
    }
    if (!scene.create(device, data, resolved, &error, tiles.get())) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return motion.start(scene, tiles.get(), lapse, jobs, &error);
  }

  Vector<TerrainTile> held;  // `follow`'s, kept, as a host's ring keeps its tiles

  // A frame's world update, as a host's ring hands it over: the tiles round the camera.
  void follow(const Camera& camera, f64 dt = 1.0 / 60.0) {
    if (tiles != nullptr) {
      terrain_tiles_round(tiles->tiles_desc(), camera.position, held);  // as MovingGround does
      tiles->set_tiles(std::span<const TerrainTile>(held.data(), held.size()));
    }
    motion.frame(dt, camera.position);
  }
};

TimeLapseConfig still_lapse() {
  TimeLapseConfig lapse;
  lapse.rate = 0.0;
  lapse.wait = true;
  return lapse;
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

// The camera's eye as `unproject` adds it back.
Vec3 eye_of(const Camera& camera) { return relative(camera.position, WorldPos::origin()); }

// A camera at and looking at two float32 points of the world, as the cases name them.
Camera camera_between(Vec3 position, Vec3 target) {
  Camera camera;
  camera.position = absolute(WorldPos::origin(), position);
  camera.target = absolute(WorldPos::origin(), target);
  return camera;
}

Vec3 normal_of(const CapturedFrame& shot, u32 p) {
  const u8* n = shot.normals.data() + u64{p} * 3u;
  const Vec3 v{static_cast<f32>(n[0]) / 127.5f - 1.0f, static_cast<f32>(n[1]) / 127.5f - 1.0f,
               static_cast<f32>(n[2]) / 127.5f - 1.0f};
  return normalize(v);
}

// What a view of the ground found (`census`): holes, pixels off the surface, and the shading
// normal between neighbouring pixels — inside one tile, across a border between two tiles of one
// level, and across one between two levels. A lighting seam is a **discontinuity**: a step across
// the border that the steps just before and just after it along the same row or column do not
// have. So each pair's step is kept with its **excess** over the larger of its two neighbouring
// pairs' (the pair ending where it starts, and the one starting where it ends): a normal field
// that is continuous, however it kinks at a triangle's edge, steps across a border about as much
// as beside it, and a seam steps by its own jump wherever its neighbours are.
struct Census {
  u32 covered = 0;
  u32 uncovered = 0;
  u32 holes = 0;  // an uncovered pixel under a covered one in its column: a crack
  u32 off_surface = 0;
  f64 worst_off_m = 0.0;
  Vector<f32> inside_steps;
  Vector<f32> same_level_steps;
  Vector<f32> cross_level_steps;
  Vector<f32> inside_excess;
  Vector<f32> same_level_excess;
  Vector<f32> cross_level_excess;
  u32 cross_level_pairs = 0;
  u32 same_level_pairs = 0;
};

f32 percentile(Vector<f32> v, f64 q) {
  if (v.empty()) return 0.0f;
  std::sort(v.begin(), v.end());
  const u32 k = static_cast<u32>(q * static_cast<f64>(v.size() - 1));
  return v[k];
}

// The surface model at a point: the tile source at the finest level's pair and blend, as the pool
// blends it, and the local relief a pixel's reconstruction may stand within — the range of the
// surface over the coarsest lattice's cell round the point, where a fan along a coarser tile's edge
// may reach.
struct SurfaceModel {
  const scene_gen::TileSource* source = nullptr;
  f64 time_a = 0.0;
  f64 time_b = 0.0;
  f32 blend = 0.0f;
  i64 fine_mm = 500;
  i64 coarse_mm = 2000;

  f32 at_lattice(i64 i, i64 j) const {
    f32 a = 0.0f;
    f32 b = 0.0f;
    (void)source->heights(time_a, fine_mm, static_cast<i32>(i), static_cast<i32>(j), 1, 1,
                          std::span<f32>(&a, 1));
    if (time_b > time_a) {
      (void)source->heights(time_b, fine_mm, static_cast<i32>(i), static_cast<i32>(j), 1, 1,
                            std::span<f32>(&b, 1));
      return a * (1.0f - blend) + b * blend;
    }
    return a;
  }
  // The lowest and highest of the surface on the fine lattice over the coarse cell round (x, z),
  // and one fine sample beyond it.
  void bounds(f32 x, f32 z, f32& lo, f32& hi) const {
    const i64 r = coarse_mm / fine_mm;
    const i64 ci =
        static_cast<i64>(std::floor(static_cast<f64>(x) * 1000.0 / static_cast<f64>(coarse_mm)));
    const i64 cj =
        static_cast<i64>(std::floor(static_cast<f64>(z) * 1000.0 / static_cast<f64>(coarse_mm)));
    lo = std::numeric_limits<f32>::max();
    hi = -std::numeric_limits<f32>::max();
    for (i64 j = cj * r - 1; j <= (cj + 1) * r + 1; ++j) {
      for (i64 i = ci * r - 1; i <= (ci + 1) * r + 1; ++i) {
        const f32 h = at_lattice(i, j);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
      }
    }
  }
};

Census census(const CapturedFrame& shot, const Mat4& inverse_view_proj, Vec3 eye,
              const TerrainTileSet& set, const SurfaceModel* model, u32 model_stride = 7,
              f32 pair_m = 0.25f) {
  Census c;
  const u32 w = shot.width;
  const u32 h = shot.height;
  const f64 t = static_cast<f64>(set.tiles_desc().tile_size);
  Vector<Vec3> world(u64{w} * h);
  Vector<u8> has(u64{w} * h, u8{0});
  Vector<i32> level(u64{w} * h, -1);
  Vector<i64> tile(u64{w} * h, 0);
  for (u32 py = 0; py < h; ++py) {
    for (u32 px = 0; px < w; ++px) {
      const u32 p = py * w + px;
      if (!unproject(shot, inverse_view_proj, eye, px, py, world[p])) {
        ++c.uncovered;
        continue;
      }
      has[p] = 1;
      ++c.covered;
      const i32 tx = static_cast<i32>(std::floor(static_cast<f64>(world[p].x) / t));
      const i32 tz = static_cast<i32>(std::floor(static_cast<f64>(world[p].z) / t));
      tile[p] = (static_cast<i64>(tx) << 32) ^ static_cast<i64>(static_cast<u32>(tz));
      u32 l = 0;
      u32 index = 0;
      if (set.find(tx, tz, l, index)) {
        level[p] = static_cast<i32>(l);
      } else if (const u8 far = set.far_level_at(set.layout(), tx, tz); far != 0) {
        // A far level's (renderer.md, "Ground to the horizon"): its own tile, larger than a world
        // tile, so a world tile's border inside it is no border.
        level[p] = far;
        const f64 ft = static_cast<f64>(set.far_tile_mm(far)) / 1000.0;
        const i32 fx = static_cast<i32>(std::floor(static_cast<f64>(world[p].x) / ft));
        const i32 fz = static_cast<i32>(std::floor(static_cast<f64>(world[p].z) / ft));
        tile[p] = (static_cast<i64>(far) << 58) ^ (static_cast<i64>(fx) << 29) ^
                  static_cast<i64>(static_cast<u32>(fz) & 0x1FFFFFFFu);
      }
    }
  }
  // Holes: an uncovered pixel with ground above it in its column (the camera is above the ground,
  // and a heightfield has no overhang, so the ground fills every column from the bottom up to its
  // silhouette).
  for (u32 px = 0; px < w; ++px) {
    bool ground_above = false;
    for (u32 py = 0; py < h; ++py) {
      const u32 p = py * w + px;
      if (has[p] != 0) {
        ground_above = true;
      } else if (ground_above) {
        ++c.holes;
      }
    }
  }
  // Every covered pixel on the surface, within the relief its fans may span (a sample of them). A
  // far level's pixel is not a ring's tile (`find` knows only those) and its heights are filtered
  // to its spacing rather than the model's lattice: its surface is held by the tiling test and the
  // seams below.
  if (model != nullptr) {
    for (u32 p = 0; p < u64{w} * h; p += model_stride) {
      if (has[p] == 0 || level[p] < 0 || set.is_far(static_cast<u32>(level[p]))) continue;
      f32 lo = 0.0f;
      f32 hi = 0.0f;
      model->bounds(world[p].x, world[p].z, lo, hi);
      const f32 y = world[p].y;
      const f64 off = y < lo ? static_cast<f64>(lo - y) : y > hi ? static_cast<f64>(y - hi) : 0.0;
      if (off > 0.005) ++c.off_surface;
      c.worst_off_m = std::max(c.worst_off_m, off);
    }
  }
  // The normal's step between each pixel and its right and lower neighbours, by what lies between,
  // and its excess over the steps on either side of it along the same line.
  const auto step_of = [&](u32 p, u32 q, f32& out) {
    if (has[p] == 0 || has[q] == 0) return false;
    // Neighbours a pixel apart on the ground, not across a silhouette (pair_m: a pixel's footprint
    // on the ground, larger in a view of the far levels).
    if (length(world[p] - world[q]) > pair_m) return false;
    out = std::acos(std::clamp(dot(normal_of(shot, p), normal_of(shot, q)), -1.0f, 1.0f));
    return true;
  };
  for (u32 py = 1; py + 2 < h; ++py) {
    for (u32 px = 1; px + 2 < w; ++px) {
      const u32 p = py * w + px;
      for (const u32 d : {1u, w}) {
        const u32 q = p + d;
        f32 step = 0.0f;
        f32 before = 0.0f;
        f32 after = 0.0f;
        if (!step_of(p, q, step) || !step_of(p - d, p, before) || !step_of(q, q + d, after))
          continue;
        // The neighbouring pairs must each lie within one tile, so they are the surface on either
        // side of this pair and not another border.
        if (tile[p - d] != tile[p] || tile[q] != tile[q + d]) continue;
        const f32 excess = step - std::max(before, after);
        if (tile[p] == tile[q]) {
          c.inside_steps.push_back(step);
          c.inside_excess.push_back(excess);
        } else if (level[p] == level[q]) {
          c.same_level_steps.push_back(step);
          c.same_level_excess.push_back(excess);
          ++c.same_level_pairs;
        } else {
          c.cross_level_steps.push_back(step);
          c.cross_level_excess.push_back(excess);
          ++c.cross_level_pairs;
        }
      }
    }
  }
  return c;
}

// `lattice_step` is, for a coarser cut, the largest step inside a tile at the finest cut of the
// same view: the steepest the lattice's own triangles turn the normal between two pixels (0 at the
// finest cut itself, where `c` says so).
void check_seams(const Census& c, const std::string& what, f32 lattice_step = 0.0f) {
  INFO(what);
  MESSAGE(
      what << ": " << c.covered << " covered, " << c.uncovered << " uncovered, " << c.holes
           << " holes, " << c.off_surface << " off the surface (worst " << c.worst_off_m
           << " m); the normal's step, 99th percentile and largest, and its excess over the "
           << "steps beside it, largest: inside a tile " << percentile(c.inside_steps, 0.99) << ", "
           << percentile(c.inside_steps, 1.0) << ", " << percentile(c.inside_excess, 1.0)
           << "; across one level's borders " << percentile(c.same_level_steps, 0.99) << ", "
           << percentile(c.same_level_steps, 1.0) << ", " << percentile(c.same_level_excess, 1.0)
           << " (" << c.same_level_pairs << " pairs); across two levels' "
           << percentile(c.cross_level_steps, 0.99) << ", " << percentile(c.cross_level_steps, 1.0)
           << ", " << percentile(c.cross_level_excess, 1.0) << " (" << c.cross_level_pairs
           << " pairs)");
  CHECK(c.holes == 0);
  CHECK(c.off_surface == 0);
  CHECK(c.same_level_pairs > 0);
  CHECK(c.cross_level_pairs > 0);
  // No lighting seam: the normal steps across a border no more than it does anywhere inside a tile,
  // and it jumps there — steps more than on either side of it — no more than it does across a
  // triangle's edge inside one (a quantum of the 8-bit normal channel spare, about 0.016 radians).
  //
  // **At a coarser cut the step is bounded by the lattice's own as well** (2026-10-06): the two
  // tiles of a border are cut apart, so one may draw the lattice's own triangles there — on a
  // crest, where they turn the normal fastest — and the other a coarser cluster, and that pair
  // steps by the fine side's gradient, which the coarse cut's own tiles need not show anywhere
  // inside. The finest cut of the same view measures it. Found when the tiles' UVs became relative
  // to their corners and a tile's DAG stopped depending on where it is: 50 km out, from above, at a
  // pixel's cut, one pair across a same-level border at x = 50,000 m stepped by 0.1126 rad — the
  // flat coarse side 0.008 before it, the fine side 0.033 after — where the coarse cut's largest
  // step inside a tile was 0.0917 and the finest cut's 0.1316; its excess, 0.080, is within the
  // inside excess's 0.070 and a quantum. (The old DAGs, simplified from UVs of 390 there, had a
  // step of 0.206 inside a tile at that cut, which the bound had been measured against.)
  constexpr f32 k_quantum = 0.016f;
  const f32 step_bound = std::max(percentile(c.inside_steps, 1.0), lattice_step) + k_quantum;
  CHECK(percentile(c.same_level_steps, 1.0) <= step_bound);
  CHECK(percentile(c.cross_level_steps, 1.0) <= step_bound);
  CHECK(percentile(c.same_level_excess, 1.0) <= percentile(c.inside_excess, 1.0) + k_quantum);
  CHECK(percentile(c.cross_level_excess, 1.0) <= percentile(c.inside_excess, 1.0) + k_quantum);
}

// A view of the ground from above that straddles every level's borders round `centre`.
Camera looking_down_at(Vec3 centre, f32 height = 26.0f) {
  Camera camera;
  camera.position = absolute(WorldPos::origin(), Vec3{centre.x - 3.0f, height, centre.z + 9.0f});
  camera.target = absolute(WorldPos::origin(), Vec3{centre.x + 1.0f, 0.0f, centre.z - 2.0f});
  camera.znear = 0.5f;
  return camera;
}
// A grazing one, a couple of metres over the sand, looking out over the rings.
Camera looking_across(Vec3 eye_ground, f32 ground_y) {
  Camera camera;
  camera.position = absolute(WorldPos::origin(), Vec3{eye_ground.x, ground_y + 2.5f, eye_ground.z});
  camera.target = absolute(WorldPos::origin(),
                           Vec3{eye_ground.x + 30.0f, ground_y - 4.0f, eye_ground.z - 18.0f});
  camera.znear = 0.1f;
  return camera;
}

}  // namespace

TEST_CASE("world tiles: the erg from tiles is the grid's picture inside its extent, and goes on") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!std::filesystem::exists(path)) {
    MESSAGE("the desert-erg scene is not here; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_tiles_erg"};
  SceneDesc erg;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, erg, error), error);
  // The erg's own field (its bands, wind and time) on a grid of its own spacing, 1.5 m, over
  // 384 m: what a tile at the same spacing is to be held to.
  erg.terrain.size = 257;
  erg.terrain.extent = 192.0f;
  erg.terrain.has_detail = false;
  erg.ddc = slashes(tmp.native() / "ddc");
  erg.camera_path.clear();
  // 48 m tiles of 32 cells: 1.5 m, the grid's lattice (its corner, -192 m, is a lattice point), in
  // one ring wide enough that every view below has tiles over the whole of the grid it sees.
  TerrainTilesDesc t;
  t.tile_size = 48.0f;
  t.ring_count = 1;
  t.radius[0] = 10.5f;
  t.cells[0] = 32;
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 320;
  constexpr u32 k_height = 200;
  const Camera views[3] = {
      camera_between(Vec3{-40.0f, 90.0f, 60.0f}, Vec3{30.0f, 0.0f, -40.0f}),
      camera_between(Vec3{120.0f, 40.0f, -150.0f}, Vec3{40.0f, 10.0f, -60.0f}),
      camera_between(Vec3{0.0f, 250.0f, 0.1f}, Vec3{0.0f, 0.0f, 0.0f}),
  };
  TileRig grid;
  REQUIRE_MESSAGE(grid.build(gpu.device, erg, settings, k_width, k_height, still_lapse(), &pool,
                             nullptr, WorldPos::origin()),
                  grid.error);
  TileRig tiles;
  REQUIRE_MESSAGE(tiles.build(gpu.device, erg, settings, k_width, k_height, still_lapse(), &pool,
                              &t, WorldPos::origin()),
                  tiles.error);
  CaptureChannels channels;
  channels.depth = true;
  channels.ids = true;
  u32 compared = 0;
  u32 depth_differ = 0;
  u32 depth_bits = 0;  // pixels whose depth differs in any bit
  u32 worst_ulps = 0;
  constexpr u32 k_depth_ulps = 16;
  u32 color_differ = 0;
  u32 beyond = 0;
  u32 diagnosed = 0;
  for (const Camera& camera : views) {
    FrameDesc frame;
    frame.camera = camera;
    frame.lod_px = 0.0f;  // the finest clusters: the lattice's own triangles, in both
    grid.follow(camera);
    tiles.follow(camera);
    CapturedFrame a;
    CapturedFrame b;
    REQUIRE_MESSAGE(grid.renderer.capture(frame, channels, a, &grid.error), grid.error);
    REQUIRE_MESSAGE(tiles.renderer.capture(frame, channels, b, &tiles.error), tiles.error);
    const Mat4 inverse_view_proj = inverse(grid.renderer.views()[0].view_proj);
    for (u32 py = 0; py < k_height; ++py) {
      for (u32 px = 0; px < k_width; ++px) {
        const u32 p = py * k_width + px;
        Vec3 world;
        const bool in_grid = unproject(a, inverse_view_proj, eye_of(camera), px, py, world);
        if (!in_grid) {
          beyond += b.depth[p] > 0.0f ? 1u : 0u;  // ground the tiles have past the grid
          continue;
        }
        // Inside the grid, two samples from its edge, where its normals are one-sided.
        if (std::abs(world.x) > 189.0f || std::abs(world.z) > 189.0f) continue;
        ++compared;
        // **To a few float steps, not to the bit** (renderer.md, "The ground's tiles are placed at
        // their corners"): the grid's vertices are world coordinates under an instance at cell
        // zero and a tile's are metres from its corner under an instance at that corner, so the
        // same lattice point reaches the frame through two different roundings, each within a
        // float step at its distance from the eye.
        u32 da = 0;
        u32 db = 0;
        std::memcpy(&da, &a.depth[p], 4);
        std::memcpy(&db, &b.depth[p], 4);
        const u32 ulps = da > db ? da - db : db - da;
        worst_ulps = std::max(worst_ulps, ulps);
        depth_bits += ulps != 0 ? 1u : 0u;
        const bool depth_off = ulps > k_depth_ulps;
        depth_differ += depth_off ? 1u : 0u;
        u32 worst_byte = 0;
        for (u32 k = 0; k < 4; ++k) {
          const i32 d =
              static_cast<i32>(a.color[u64{p} * 4 + k]) - static_cast<i32>(b.color[u64{p} * 4 + k]);
          worst_byte = std::max<u32>(worst_byte, static_cast<u32>(d < 0 ? -d : d));
        }
        color_differ += worst_byte > 0 ? 1u : 0u;
        if ((depth_off || worst_byte > 0) && diagnosed < 12) {
          ++diagnosed;
          // Where it is: how far from a tile's border (48 m) and from a lattice line (1.5 m).
          const f64 tb = std::min(std::abs(std::remainder(static_cast<f64>(world.x), 48.0)),
                                  std::abs(std::remainder(static_cast<f64>(world.z), 48.0)));
          const f64 lb = std::min(std::abs(std::remainder(static_cast<f64>(world.x), 1.5)),
                                  std::abs(std::remainder(static_cast<f64>(world.z), 1.5)));
          MESSAGE("differs at (" << world.x << ", " << world.y << ", " << world.z << "): depth "
                                 << a.depth[p] << " against " << b.depth[p] << ", colour by "
                                 << worst_byte << "; " << tb << " m from a tile border, " << lb
                                 << " m from a lattice line; ids " << a.ids[u64{p} * k_id_words]
                                 << "/" << a.ids[u64{p} * k_id_words + 1] << " and "
                                 << b.ids[u64{p} * k_id_words] << "/"
                                 << b.ids[u64{p} * k_id_words + 1]);
        }
      }
    }
  }
  MESSAGE("the erg at 1.5 m: " << compared << " pixels inside the grid compared, " << depth_differ
                               << " differ in depth by more than " << k_depth_ulps
                               << " float steps (" << depth_bits << " in any bit, " << worst_ulps
                               << " steps at the most), " << color_differ << " in colour; "
                               << beyond << " pixels of ground past the grid's edge");
  CHECK(compared > 50'000);
  // Past a few float steps only where a silhouette's edge moved by that rounding and a pixel went
  // to the surface behind it: 11 of 156,172 on the RTX 5090 (2026-10-05), each of the same colour.
  CHECK(depth_differ * 5'000 <= compared);
  CHECK(color_differ == 0);
  CHECK(beyond > 0);

  // And the desert goes on: two kilometres past the grid's edge, every pixel of a view down on the
  // sand is ground.
  const Camera far =
      camera_between(Vec3{2'200.0f, 120.0f, -1'700.0f}, Vec3{2'230.0f, 0.0f, -1'760.0f});
  tiles.follow(far);
  FrameDesc frame;
  frame.camera = far;
  CapturedFrame shot;
  REQUIRE_MESSAGE(tiles.renderer.capture(frame, channels, shot, &tiles.error), tiles.error);
  CHECK(shot.covered == k_width * k_height);
  CHECK(tiles.motion.ring_stats().failed == 0);
}

TEST_CASE("world tiles: no crack, no T-junction and no lighting seam at any border") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_tiles_seams"};
  const SceneDesc desc = dune_scene(slashes(tmp.native() / "ddc"));
  const TerrainTilesDesc t = small_tiles();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 256;
  constexpr u32 k_height = 192;
  CaptureChannels channels;
  channels.depth = true;
  channels.normals = true;
  channels.ids = true;
  for (const f32 origin_x : {0.0f, 50'000.0f}) {
    TileRig rig;
    const Vec3 centre{origin_x + 3.0f, 0.0f, -2.0f};
    REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, still_lapse(), &pool,
                              &t, absolute(WorldPos::origin(), centre)),
                    rig.error);
    REQUIRE(rig.tiles->level_count() == 4);
    const scene_gen::TileSource source = rig.ground->provider().tiles();
    SurfaceModel model;
    model.source = &source;
    model.time_a = model.time_b = desc.terrain.time_s;
    model.fine_mm = 500;
    model.coarse_mm = 2000;
    for (const bool grazing : {false, true}) {
      const Camera camera = grazing ? looking_across(centre, rig.ground->height(centre.x, centre.z))
                                    : looking_down_at(centre);
      rig.follow(camera);
      FrameDesc frame;
      frame.camera = camera;
      f32 lattice_step = 0.0f;  // the finest cut's largest step inside a tile, for the coarser one
      for (const f32 lod : {0.0f, 1.0f}) {  // the finest cut, and the default's coarser one
        frame.lod_px = lod;
        CapturedFrame shot;
        REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
        const Census c = census(shot, inverse(rig.renderer.views()[0].view_proj), eye_of(camera),
                                *rig.tiles, lod == 0.0f ? &model : nullptr);
        const std::string what = std::string(origin_x > 0.0f ? "50 km out, " : "at the origin, ") +
                                 (grazing ? "grazing" : "from above") +
                                 (lod == 0.0f ? ", the finest cut" : ", a pixel's cut");
        if (!grazing) CHECK(c.uncovered == 0);
        check_seams(c, what, lattice_step);
        if (lod == 0.0f) lattice_step = percentile(c.inside_steps, 1.0);
      }
    }
  }
}

namespace {

// The small world with three far levels past it (renderer.md, "Ground to the horizon"): 4 m, 8 m
// and 16 m, on far tiles of four cells, out to a square 1.5 km a side.
TerrainTilesDesc small_far_tiles() {
  TerrainTilesDesc t = small_tiles();
  t.far_levels = 3;
  t.far_cells = 4;
  t.far_ratio = 2;
  return t;
}

// The far level a world point is drawn at under `set`'s layout, when it is one and stands clear of
// everything that is not the inside of one level: no ring's tile in the 3 x 3 world tiles round it,
// and at least four of a level's cells from every far square's border. 0 otherwise.
u32 far_level_clear_at(const TerrainTileSet& set, Vec3 w) {
  const f64 t = static_cast<f64>(set.tiles_desc().tile_size);
  const i32 tx = static_cast<i32>(std::floor(static_cast<f64>(w.x) / t));
  const i32 tz = static_cast<i32>(std::floor(static_cast<f64>(w.z) / t));
  for (i32 dz = -1; dz <= 1; ++dz) {
    for (i32 dx = -1; dx <= 1; ++dx) {
      u32 l = 0;
      u32 index = 0;
      if (set.find(tx + dx, tz + dz, l, index)) return 0;
    }
  }
  const TerrainRingLayout layout = set.layout();
  for (u32 l = 1; l <= set.far_count(); ++l) {
    const f64 lo_x = static_cast<f64>(layout.cx[l] - layout.half[l]) / 1000.0;
    const f64 hi_x = static_cast<f64>(layout.cx[l] + layout.half[l]) / 1000.0;
    const f64 lo_z = static_cast<f64>(layout.cz[l] - layout.half[l]) / 1000.0;
    const f64 hi_z = static_cast<f64>(layout.cz[l] + layout.half[l]) / 1000.0;
    const f64 x = static_cast<f64>(w.x);
    const f64 z = static_cast<f64>(w.z);
    const f64 inside = std::min(std::min(x - lo_x, hi_x - x), std::min(z - lo_z, hi_z - z));
    if (std::abs(inside) < 4.0 * set.lattice(l).spacing) return 0;
  }
  return set.far_level_at(layout, tx, tz);
}

}  // namespace

TEST_CASE("world tiles: the far levels meet the rings and each other with no crack and no seam") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_tiles_far"};
  // Dunes 160 m apart and 8 m high: every band of them is one each far level's spacing carries (16
  // m at the coarsest, so a cell of 32 m or more), so a border is a seam and not also the step from
  // a band to its mean, which a far level that drops a band its neighbour keeps draws (measured in
  // the erg's flight, renderer.md "Ground to the horizon").
  SceneDesc desc = dune_scene(slashes(tmp.native() / "ddc"));
  desc.terrain.dune_wavelength = 160.0f;
  desc.terrain.dune_height = 8.0f;
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 256;
  constexpr u32 k_height = 192;
  CaptureChannels channels;
  channels.depth = true;
  channels.normals = true;
  channels.ids = true;
  channels.color = true;
  const TerrainTilesDesc near_only = small_tiles();
  const TerrainTilesDesc with_far = small_far_tiles();
  for (const f32 origin_x : {0.0f, 50'000.0f}) {
    const Vec3 centre{origin_x + 3.0f, 0.0f, -2.0f};
    TileRig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, still_lapse(), &pool,
                              &with_far, absolute(WorldPos::origin(), centre)),
                    rig.error);
    REQUIRE(rig.tiles->level_count() == 7);
    const std::string where = origin_x > 0.0f ? "50 km out" : "at the origin";
    // **Every border, far and near**: from 90 m up looking down 140 m away, across the rings' edge
    // and the first two far levels'; and from 6 m over the sand looking out to the last far
    // level's edge. No hole in any column, and the shading normal steps across a border no more
    // than inside a tile.
    const f32 ground = rig.ground->height(centre.x, centre.z);
    Camera high = camera_between(Vec3{centre.x - 20.0f, 90.0f, centre.z + 30.0f},
                                 Vec3{centre.x + 100.0f, 0.0f, centre.z - 90.0f});
    high.znear = 0.5f;
    Camera low = camera_between(Vec3{centre.x, ground + 6.0f, centre.z},
                                Vec3{centre.x + 300.0f, ground - 4.0f, centre.z - 180.0f});
    low.znear = 0.1f;
    for (const bool grazing : {false, true}) {
      const Camera camera = grazing ? low : high;
      rig.follow(camera);
      FrameDesc frame;
      frame.camera = camera;
      for (const f32 lod : {0.0f, 1.0f}) {
        frame.lod_px = lod;
        CapturedFrame shot;
        REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
        // Pixels several metres apart on the far ground are neighbours here.
        const Census c = census(shot, inverse(rig.renderer.views()[0].view_proj), eye_of(camera),
                                *rig.tiles, nullptr, 7, 6.0f);
        const std::string what = where +
                                 (grazing ? ", grazing out to the far edge" : ", from 90 m") +
                                 (lod == 0.0f ? ", the finest cut" : ", a pixel's cut");
        // No crack anywhere, and no jump of the shading normal where two levels meet beyond what a
        // triangle's edge inside a tile has. Between two far tiles of one level the census is no
        // measure: its pairs out there are metres apart, a cell of the 4 m level each, so a pair
        // across a border is a sample of the surface's own creases, and at the origin's grazing
        // view the largest of its 1,656 border pairs (0.137 radians past the steps beside it, a
        // crease at z = -64) outran the largest of the inside's (0.086); in the six-cell layout
        // below, where z = -64 is inside a tile, the inside's largest is 0.157 and the borders'
        // 0.077. The seam between two tiles of one level is held below instead, by the picture.
        INFO(what);
        MESSAGE(
            what << ": " << c.covered << " covered, " << c.holes
                 << " holes; the normal's jump, largest:"
                 << " inside a tile " << percentile(c.inside_excess, 1.0) << ", across one level's "
                 << "borders " << percentile(c.same_level_excess, 1.0) << " (" << c.same_level_pairs
                 << " pairs), across two levels' " << percentile(c.cross_level_excess, 1.0) << " ("
                 << c.cross_level_pairs << " pairs)");
        CHECK(c.holes == 0);
        CHECK(c.cross_level_pairs > 0);
        CHECK(percentile(c.cross_level_excess, 1.0) <= percentile(c.inside_excess, 1.0) + 0.016f);
      }
    }

    // **A far level's tiles leave no mark on it**: the same levels cut into far tiles of six cells
    // (24 m, then 48 m and 96 m) instead of four, so that most of one layout's borders between
    // two tiles of a level are the inside of a tile in the other, draw the same depth, normal and
    // colour at every pixel of a far level both draw clear of a ring and of a square's border, at
    // the finest cut. A seam between two tiles of one level would differ there; nothing does.
    {
      TerrainTilesDesc six = with_far;
      six.far_cells = 6;
      TileRig other;
      REQUIRE_MESSAGE(other.build(gpu.device, desc, settings, k_width, k_height, still_lapse(),
                                  &pool, &six, absolute(WorldPos::origin(), centre)),
                      other.error);
      for (const bool grazing : {false, true}) {
        const Camera camera = grazing ? low : high;
        rig.follow(camera);
        other.follow(camera);
        FrameDesc frame;
        frame.camera = camera;
        frame.lod_px = 0.0f;
        CapturedFrame a;
        CapturedFrame b;
        REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, a, &rig.error), rig.error);
        REQUIRE_MESSAGE(other.renderer.capture(frame, channels, b, &other.error), other.error);
        const Mat4 inv = inverse(rig.renderer.views()[0].view_proj);
        const Vec3 eye = eye_of(camera);
        u32 compared = 0;
        u32 borders = 0;  // compared pixels on a border of one layout's tiles of a level
        u32 differ = 0;
        u32 depth_bits = 0;  // compared pixels whose depth differs in any bit
        for (u32 p = 0; p < k_width * k_height; ++p) {
          // The point each layout draws there, both of them clear far ground of one level in both
          // layouts: the squares differ with the tiles' size, so a crest in front may be drawn at
          // another level's filtered heights in one of them and hide what the other shows.
          Vec3 w{};
          Vec3 wb{};
          if (!unproject(a, inv, eye, p % k_width, p / k_width, w) ||
              !unproject(b, inv, eye, p % k_width, p / k_width, wb))
            continue;
          const u32 la = far_level_clear_at(*rig.tiles, w);
          if (la == 0 || far_level_clear_at(*other.tiles, w) != la ||
              far_level_clear_at(*rig.tiles, wb) != la ||
              far_level_clear_at(*other.tiles, wb) != la)
            continue;
          ++compared;
          // Within half a cell of a border between two of the four-cell layout's tiles.
          const f64 ft = static_cast<f64>(rig.tiles->far_tile_mm(la)) / 1000.0;
          const f64 s = rig.tiles->lattice(la).spacing;
          const f64 fx = static_cast<f64>(w.x) / ft;
          const f64 fz = static_cast<f64>(w.z) / ft;
          borders += std::abs(fx - std::round(fx)) * ft < 0.5 * s ||
                     std::abs(fz - std::round(fz)) * ft < 0.5 * s;
          // Depth to a few float steps: the two layouts' tiles have different corners, and a tile
          // is placed at its own, so one lattice point reaches the frame through two roundings
          // (renderer.md, "The ground's tiles are placed at their corners"). Normals and colour,
          // which a float step does not reach, to the bit.
          u32 da = 0;
          u32 db = 0;
          std::memcpy(&da, &a.depth[p], sizeof(f32));
          std::memcpy(&db, &b.depth[p], sizeof(f32));
          const u32 ulps = da > db ? da - db : db - da;
          depth_bits += ulps != 0 ? 1u : 0u;
          bool same = ulps <= 16;
          for (u32 k = 0; k < 3 && same; ++k)
            same = a.normals[u64{p} * 3 + k] == b.normals[u64{p} * 3 + k];
          for (u32 k = 0; k < 4 && same; ++k)
            same = a.color[u64{p} * 4 + k] == b.color[u64{p} * 4 + k];
          differ += same ? 0u : 1u;
        }
        // The census of the other layout: what was a border is the inside of a tile there.
        const Census c = census(b, inv, eye, *other.tiles, nullptr, 7, 6.0f);
        const std::string view = where + (grazing ? ", grazing" : ", from 90 m");
        MESSAGE(view << ": far tiles of four cells against six, " << compared
                     << " far pixels compared, " << borders << " of them on a border of four's, "
                     << differ << " differ (" << depth_bits
                     << " in some bit of depth); the six's census, the normal's jump inside a tile "
                     << percentile(c.inside_excess, 1.0) << ", across one level's borders "
                     << percentile(c.same_level_excess, 1.0));
        CHECK(compared > 1'000);
        CHECK(borders > 100);
        CHECK(differ == 0);
      }
    }

    // **The rings' picture is what it was**: a view that sees no far level and none of the rings'
    // tiles beside one draws the same depth and colour, pixel for pixel, with the far levels as
    // without them.
    TileRig plain;
    REQUIRE_MESSAGE(plain.build(gpu.device, desc, settings, k_width, k_height, still_lapse(), &pool,
                                &near_only, absolute(WorldPos::origin(), centre)),
                    plain.error);
    const Camera inside = looking_down_at(centre);
    rig.follow(inside);
    plain.follow(inside);
    FrameDesc frame;
    frame.camera = inside;
    frame.lod_px = 1.0f;
    CapturedFrame a;
    CapturedFrame b;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, a, &rig.error), rig.error);
    REQUIRE_MESSAGE(plain.renderer.capture(frame, channels, b, &plain.error), plain.error);
    REQUIRE(a.depth.size() == b.depth.size());
    REQUIRE(a.color.size() == b.color.size());
    u32 depth_differ = 0;
    u32 color_differ = 0;
    u32 covered = 0;
    for (u32 p = 0; p < a.depth.size(); ++p) {
      covered += a.depth[p] > 0.0f;
      depth_differ += std::memcmp(&a.depth[p], &b.depth[p], sizeof(f32)) != 0;
    }
    for (u32 k = 0; k < a.color.size(); ++k)
      color_differ += a.color[k] != b.color[k];
    MESSAGE(where << ": the rings' view with and without far levels, " << covered
                  << " pixels covered, " << depth_differ << " differ in depth and " << color_differ
                  << " colour bytes differ");
    CHECK(covered == u64{k_width} * k_height);
    CHECK(depth_differ == 0);
    CHECK(color_differ == 0);
  }
}

TEST_CASE("world tiles: in time-lapse, one surface time, each level's bound, and no seam") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_tiles_lapse"};
  const SceneDesc desc = dune_scene(slashes(tmp.native() / "ddc"));
  // With the far levels past the rings (renderer.md, "Ground to the horizon"): they share the one
  // surface time and their own bounds, a quarter of 4, 8 and 16 m a frame.
  const TerrainTilesDesc t = small_far_tiles();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.time_rate = 604'800.0;  // a game week a real second
  TimeLapseConfig lapse;
  lapse.rate = settings.time_rate;
  lapse.fraction = 0.25;
  lapse.min_step_s = 60.0;
  lapse.max_step_s = 30.0 * 86'400.0;
  lapse.wait = true;  // fields waited for: a frame's picture is a function of the frame
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 150;
  const Vec3 start{3.0f, 0.0f, -2.0f};
  TileRig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, lapse, &pool, &t,
                            absolute(WorldPos::origin(), start)),
                  rig.error);
  CaptureChannels channels;
  channels.depth = true;
  channels.normals = true;
  channels.ids = true;
  constexpr u32 k_frames = 90;
  u32 apart = 0;  // frames whose levels stood at pairs of different times
  f64 worst_ratio = 0.0;
  u32 installed = 0;
  for (u32 f = 0; f < k_frames; ++f) {
    // The camera slides 12 cm a frame, so tiles change ring and rebuild as the sand moves.
    Vec3 centre = start;
    centre.x += 0.12f * static_cast<f32>(f);
    const Camera camera = looking_down_at(centre);
    rig.follow(camera);
    const u32 n = rig.motion.level_count();
    f64 surface = 0.0;
    for (u32 k = 1; k < n; ++k) {
      const TerrainMotion::LevelStats s = rig.motion.level_stats(k);
      if (k == 1) surface = s.surface_s;
      CHECK(s.surface_s == surface);
      // No vertex moved more than a quarter of its own level's spacing this frame.
      const f64 bound = lapse.fraction * rig.tiles->lattice(k).spacing;
      worst_ratio = std::max(worst_ratio, s.frame_move_m / bound);
    }
    const TerrainMotion::LevelStats fine = rig.motion.level_stats(n - 1);
    const TerrainMotion::LevelStats coarse = rig.motion.level_stats(1);
    if (fine.time_a != coarse.time_a || fine.time_b != coarse.time_b) ++apart;
    if (f % 15 != 14) continue;
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = f;
    frame.lod_px = 0.0f;
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
    const Census c = census(shot, inverse(rig.renderer.views()[0].view_proj), eye_of(camera),
                            *rig.tiles, nullptr);
    CHECK(c.uncovered == 0);
    check_seams(c, "frame " + std::to_string(f));
  }
  for (u32 k = 1; k < rig.motion.level_count(); ++k)
    installed += rig.motion.level_stats(k).installed;
  MESSAGE("time-lapse over tiles: " << installed << " fields installed, " << apart << " of "
                                    << k_frames << " frames with levels at different pairs, the "
                                    << "largest move a frame " << worst_ratio
                                    << " of its level's bound; " << rig.motion.ring_stats().swaps
                                    << " rebuilds swapped in");
  CHECK(installed > 3);
  CHECK(apart > 0);
  CHECK(worst_ratio <= 1.0 + 1.0e-9);
  CHECK(rig.motion.ring_stats().failed == 0);
  CHECK(rig.motion.ring_stats().swaps > 0);
  // A far level's cadence is the bands its spacing carries and a quarter of that spacing: the
  // coarsest takes far fewer fields than the finest ring, each pair spanning longer.
  const TerrainMotion::LevelStats coarsest = rig.motion.level_stats(1);
  const TerrainMotion::LevelStats finest = rig.motion.level_stats(rig.motion.level_count() - 1);
  MESSAGE("fields installed: the coarsest far level " << coarsest.installed << ", the finest ring "
                                                      << finest.installed);
  CHECK(rig.tiles->is_far(1));
  CHECK(coarsest.installed < finest.installed);
}

namespace {

// **A tile set built ahead**, the second source (world.md, "An authored world"): each tile's
// heights at one spacing with a point of apron, written into a derived-data cache as
// `<ddc>/terrain_tiles/<key>.tile` — a key over the set's name, the tile and the spacing — and read
// back from there. It answers a window from the tiles it covers, and nothing about where they came
// from reaches the renderer.
struct BuiltTileSet {
  std::string ddc;
  u64 name = 0;
  i64 tile_mm = 8000;
  i64 spacing_mm = 500;  // the finest it was built at; coarser lattices are part of it
  u32 cells = 16;
  // Read tiles, by key: a real set would keep a bounded cache; the test's set is small. A source is
  // asked from several jobs at once (tile_source.h), so the cache is behind a lock and a tile, once
  // read, never moves.
  mutable std::mutex mutex;
  mutable Vector<std::pair<u64, std::unique_ptr<Vector<f32>>>> loaded;
  mutable u32 reads = 0;
  // **Its mip chain** (renderer.md, "Ground to the horizon"; world.md, "An authored world"): the
  // coarser lattices it was built with beside the finest, each one's heights filtered to its own
  // spacing, in tiles of the same `cells` points a side — what a far level of that spacing reads.
  // A filter it was not built with is refused.
  Vector<i64> chain;

  u64 key_of(i32 x, i32 z, i64 spacing = 0) const noexcept {
    // The finest's key as it always was; a chain level's names its spacing as a filter too.
    const i64 s = spacing == 0 ? spacing_mm : spacing;
    u64 key =
        hash_combine(hash_combine(name, static_cast<u64>(static_cast<u32>(x))),
                     hash_combine(static_cast<u64>(static_cast<u32>(z)), static_cast<u64>(s)));
    if (spacing != 0) key = hash_combine(key, 0x6d69705f636861ull);  // "mip_cha"
    return key;
  }
  std::string path_of(u64 key) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
    return ddc + "/terrain_tiles/" + hex + ".tile";
  }
  const Vector<f32>* tile(i32 x, i32 z, i64 spacing = 0) const {
    const u64 key = key_of(x, z, spacing);
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [k, h] : loaded)
      if (k == key) return h.get();
    std::string bytes;
    if (io::read_file(path_of(key), bytes) != io::Status::Ok) return nullptr;
    const u32 a = cells + 3;
    if (bytes.size() != u64{a} * a * sizeof(f32)) return nullptr;
    auto h = std::make_unique<Vector<f32>>(a * a);
    std::memcpy(h->data(), bytes.data(), bytes.size());
    ++reads;
    loaded.push_back({key, std::move(h)});
    return loaded.back().second.get();
  }
  static bool heights(const void* state, f64, i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0, u32 nx,
                      u32 nz, u32 begin, u32 end, std::span<f32> out) noexcept {
    const auto& self = *static_cast<const BuiltTileSet*>(state);
    if (out.size() != static_cast<usize>(nx) * nz) return false;
    // A lattice filtered to its spacing reads the chain level built at it, point for point; the
    // ground at a point reads the finest, every r-th point of it.
    i64 level = 0;
    if (filter_mm > 0) {
      if (filter_mm != spacing_mm ||
          std::find(self.chain.begin(), self.chain.end(), spacing_mm) == self.chain.end())
        return false;
      level = spacing_mm;
    } else if (spacing_mm % self.spacing_mm != 0) {
      return false;
    }
    const i64 r = level != 0 ? 1 : spacing_mm / self.spacing_mm;
    const u32 per_side = (nx + scene_gen::k_height_block - 1) / scene_gen::k_height_block;
    for (u32 b = begin; b < end; ++b) {
      const u32 bx = (b % per_side) * scene_gen::k_height_block;
      const u32 bz = (b / per_side) * scene_gen::k_height_block;
      for (u32 z = bz; z < std::min(nz, bz + scene_gen::k_height_block); ++z) {
        for (u32 x = bx; x < std::min(nx, bx + scene_gen::k_height_block); ++x) {
          // The point on the set's own lattice, the tile holding it, and its place in that tile's
          // heights (which carry a point of apron, so a tile's own edge is in it either way).
          const i64 fi = (i0 + static_cast<i64>(x)) * r;
          const i64 fj = (j0 + static_cast<i64>(z)) * r;
          const i64 c = self.cells;
          const i32 tx = static_cast<i32>(fi >= 0 ? fi / c : -((-fi + c - 1) / c));
          const i32 tz = static_cast<i32>(fj >= 0 ? fj / c : -((-fj + c - 1) / c));
          const Vector<f32>* h = self.tile(tx, tz, level);
          f32 v = 0.0f;  // outside the set: nothing was built there
          if (h != nullptr) {
            const i64 li = fi - static_cast<i64>(tx) * c + 1;
            const i64 lj = fj - static_cast<i64>(tz) * c + 1;
            v = (*h)[static_cast<u32>(lj) * (self.cells + 3) + static_cast<u32>(li)];
          }
          out[static_cast<usize>(z) * nx + x] = v;
        }
      }
    }
    return true;
  }
};
constexpr scene_gen::TileSourceOps k_built_ops{.heights = &BuiltTileSet::heights};

}  // namespace

TEST_CASE("world tiles: a tile set built ahead and read back draws what the ground draws") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_tiles_built"};
  const SceneDesc desc = dune_scene(slashes(tmp.native() / "ddc"));
  // The rings and three far levels: the far levels read the set's mip chain.
  const TerrainTilesDesc t = small_far_tiles();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 256;
  constexpr u32 k_height = 192;
  const Vec3 centre{3.0f, 0.0f, -2.0f};
  // The procedural picture.
  TileRig procedural;
  REQUIRE_MESSAGE(procedural.build(gpu.device, desc, settings, k_width, k_height, still_lapse(),
                                   &pool, &t, absolute(WorldPos::origin(), centre)),
                  procedural.error);
  // "The content build": every tile of a square round the centre, at the finest ring's spacing and
  // a point of apron, from the ground at the scene's own time, written into the cache by key.
  BuiltTileSet set;
  set.ddc = slashes(tmp.native() / "ddc");
  set.name = hash_bytes("desert test set", 15);
  set.tile_mm = 8000;
  set.spacing_mm = 500;
  set.cells = 16;
  std::filesystem::create_directories(tmp.native() / "ddc" / "terrain_tiles");
  const scene_gen::TileSource ground = procedural.ground->provider().tiles();
  u32 written = 0;
  // (Wide enough for the rings round the high camera too, 20 m west and 30 m south of the centre.)
  for (i32 x = -14; x <= 14; ++x) {
    for (i32 z = -14; z <= 14; ++z) {
      const u32 a = set.cells + 3;
      Vector<f32> h(u64{a} * a);
      REQUIRE(ground.heights(desc.terrain.time_s, set.spacing_mm, x * 16 - 1, z * 16 - 1, a, a,
                             std::span<f32>(h.data(), h.size())));
      const std::string_view bytes(reinterpret_cast<const char*>(h.data()), h.size() * sizeof(f32));
      REQUIRE(io::write_file(set.path_of(set.key_of(x, z)), bytes) == io::Status::Ok);
      ++written;
    }
  }
  // "The content build's" mip chain: each far level's lattice over its square and a tile beyond,
  // filtered to its spacing — here the ground's own filtered answer, which is what a build that
  // filters an authored heightfield would write (world.md, "An authored world").
  u32 chain_written = 0;
  for (u32 k = 1; k <= t.far_levels; ++k) {
    const TerrainFarLevel f = terrain_far_level(t, k);
    set.chain.push_back(f.spacing);
    const i64 span = f.spacing * set.cells;
    const i32 n = static_cast<i32>((f.half + f.snap) / span) + 1;
    for (i32 x = -n; x < n; ++x) {
      for (i32 z = -n; z < n; ++z) {
        const u32 a = set.cells + 3;
        Vector<f32> h(u64{a} * a);
        REQUIRE(ground.filtered(
            desc.terrain.time_s, f.spacing, f.spacing, x * static_cast<i32>(set.cells) - 1,
            z * static_cast<i32>(set.cells) - 1, a, a, std::span<f32>(h.data(), h.size())));
        const std::string_view bytes(reinterpret_cast<const char*>(h.data()),
                                     h.size() * sizeof(f32));
        REQUIRE(io::write_file(set.path_of(set.key_of(x, z, f.spacing)), bytes) == io::Status::Ok);
        ++chain_written;
      }
    }
  }
  // Read back through the same seam: the renderer is handed a source and nothing else.
  const scene_gen::TileSource built{&k_built_ops, &set};
  {
    // A filter the set was not built with is refused, not answered with the wrong lattice.
    f32 h[4] = {};
    CHECK_FALSE(built.filtered(desc.terrain.time_s, 3000, 3000, 0, 0, 2, 2, std::span<f32>(h, 4)));
    CHECK(built.filtered(desc.terrain.time_s, 4000, 4000, 0, 0, 2, 2, std::span<f32>(h, 4)));
  }
  TileRig authored;
  REQUIRE_MESSAGE(authored.build(gpu.device, desc, settings, k_width, k_height, still_lapse(),
                                 &pool, &t, absolute(WorldPos::origin(), centre), &built),
                  authored.error);
  CaptureChannels channels;
  channels.depth = true;
  u32 compared = 0;
  u32 differ = 0;
  u32 far_total = 0;
  // Over the rings' edge and the far levels.
  Camera high = camera_between(Vec3{centre.x - 20.0f, 90.0f, centre.z + 30.0f},
                               Vec3{centre.x + 100.0f, 0.0f, centre.z - 90.0f});
  high.znear = 0.5f;
  const Camera cameras[3] = {looking_down_at(centre),
                             looking_across(centre, procedural.ground->height(centre.x, centre.z)),
                             high};
  for (const Camera& camera : cameras) {
    procedural.follow(camera);
    authored.follow(camera);
    FrameDesc frame;
    frame.camera = camera;
    CapturedFrame a;
    CapturedFrame b;
    REQUIRE_MESSAGE(procedural.renderer.capture(frame, channels, a, &procedural.error),
                    procedural.error);
    REQUIRE_MESSAGE(authored.renderer.capture(frame, channels, b, &authored.error), authored.error);
    u32 view_differ = 0;
    u32 near_differ = 0;
    u32 far_differ = 0;
    f32 far_x = 0.0f;
    f32 far_z = 0.0f;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      if (!(a.depth[p] > 0.0f)) continue;
      ++compared;
      bool same = a.depth[p] == b.depth[p];
      for (u32 k = 0; k < 4 && same; ++k)
        same = a.color[u64{p} * 4 + k] == b.color[u64{p} * 4 + k];
      differ += same ? 0u : 1u;
      view_differ += same ? 0u : 1u;
      if (!same) {
        Vec3 w{};
        if (unproject(a, inverse(procedural.renderer.views()[0].view_proj), eye_of(camera),
                      p % k_width, p / k_width, w)) {
          u32 l = 0;
          u32 index = 0;
          const i32 tx = static_cast<i32>(std::floor(w.x / 8.0f));
          const i32 tz = static_cast<i32>(std::floor(w.z / 8.0f));
          if (procedural.tiles->find(tx, tz, l, index)) {
            ++near_differ;
          } else {
            ++far_differ;
            far_x = w.x;
            far_z = w.z;
          }
        }
      }
    }
    far_total += far_differ;
    // **A far tile's rest heights are its level's filtered answer**, as its fields are: with the
    // sand standing still the padding the fields measure against them is nothing, from either
    // source. Until 2026-10-03 a far tile was built from the ground at each point: 0.97, 0.57 and 0
    // m from the procedural source, and 3.1 m from the set built ahead, which answered zeros past
    // its finest tiles — and that set's rings drew 10,733 pixels of the grazing view differently.
    for (u32 k = 1; k < procedural.motion.level_count(); ++k) {
      CAPTURE(k);
      CHECK(procedural.motion.level_stats(k).padding_m == 0.0);
      CHECK(authored.motion.level_stats(k).padding_m == 0.0);
      CHECK(procedural.motion.level_stats(k).last_hash == authored.motion.level_stats(k).last_hash);
    }
    MESSAGE("a view: " << view_differ << " differ, " << near_differ << " on the rings' tiles, "
                       << far_differ << " on far ones, the last at " << far_x << ", " << far_z);
  }
  MESSAGE("a tile set built ahead: " << written << " tiles and " << chain_written
                                     << " of its mip chain written, " << set.reads << " read back, "
                                     << compared << " pixels compared, " << differ << " differ");
  CHECK(chain_written > 0);
  CHECK(set.reads > 0);
  CHECK(compared > 40'000);
  // The far levels draw from the chain what they draw from the ground, and the rings beside them
  // what they draw beside the ground's far levels.
  CHECK(far_total == 0);
  CHECK(differ == 0);
}

TEST_CASE("world tiles: a long flight keeps what is resident bounded and drops nothing") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_tiles_flight"};
  const SceneDesc desc = dune_scene(slashes(tmp.native() / "ddc"));
  // With the far levels (renderer.md, "Ground to the horizon"), whose squares move seven times in
  // the second half, so that their rebuilds and swaps are counted too. They made 5 allocations
  // there until 2026-10-03, every one the arenas' free lists reaching a new length as a far
  // square's move freed a strip of tiles at once (`GpuScene::give_range`), 4 of them on the frames
  // a far square moved; the lists are sized for the most they can hold since.
  const TerrainTilesDesc t = small_far_tiles();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 160;
  constexpr u32 k_height = 120;
  TileRig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, still_lapse(), &pool, &t,
                            WorldPos::origin()),
                  rig.error);
  const u64 device_bytes = rig.scene.terrain_ring_bytes();
  u32 most_chunks[k_max_terrain_levels] = {};
  u32 far_moves = 0;  // in the counted half
  // Of the frame's allocations, the ones on the frames a far square moved: said apart.
  u64 far_move_allocations = 0;
  TerrainRingLayout last_layout = rig.tiles->layout();
  // **The frame loop allocates nothing in steady state**: the host's half of a frame — the tiles
  // handed over, the motion's frame with its re-centres asked for, taken, uploaded and swapped — is
  // counted on this thread alone (a tag is the calling thread's, so the workers' builds are not),
  // over the flight's second half, once every list has reached the size the flight needs.
  static const mem::TagId k_frames_tag = mem::register_tag("tile-flight-frames");
  u64 frame_allocations = 0;
  // **And it makes no buffer** (renderer.md, "What a frame waits for"): the renderer's half too —
  // each step is drawn, begun and submitted in flight as a host's frame is — counting the device
  // buffers the frame's thread makes, which the memory tags never see. Until 2026-10-04 every
  // staged tile made a staging buffer of its own and every frame that turned a slot on or off made
  // one for the records (`GpuScene::terrain_prepare`); they go through the staging ring now. The
  // memory tag stays on the host's half, as it was: the renderer's half made one engine allocation
  // a frame here, with this test's debug logging on, which is not traced yet.
  u64 frame_buffers = 0;
  u32 swaps_counted = 0;
  // 60 tiles east and 20 north, two metres a frame: every tile the flight passes comes and goes.
  constexpr u32 k_frames = 260;
  for (u32 f = 0; f < k_frames; ++f) {
    const f32 x = 2.0f * static_cast<f32>(f);
    const f32 z = 0.6f * static_cast<f32>(f);
    const u64 allocations_before = mem::stats(k_frames_tag).allocation_count;
    const u64 buffers_before = gfx::buffers_created_on_thread();
    const u32 swaps_before = rig.motion.ring_stats().swaps;
    const Camera camera = looking_down_at(Vec3{x, 0.0f, z}, 30.0f);
    {
      const mem::TagScope scope(k_frames_tag);
      rig.follow(camera);
    }
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = f;
    rig.renderer.begin_frame();
    REQUIRE_MESSAGE(rig.renderer.submit_frame(frame, &rig.error) != 0, rig.error);
    if (f >= k_frames / 2) {
      frame_buffers += gfx::buffers_created_on_thread() - buffers_before;
      const u64 made = mem::stats(k_frames_tag).allocation_count - allocations_before;
      swaps_counted += rig.motion.ring_stats().swaps - swaps_before;
      const TerrainRingLayout now = rig.tiles->layout();
      bool far_moved = false;
      for (u32 l = 1; l <= rig.tiles->far_count(); ++l) {
        const bool m = now.cx[l] != last_layout.cx[l] || now.cz[l] != last_layout.cz[l];
        far_moves += m ? 1u : 0u;
        far_moved = far_moved || m;
      }
      frame_allocations += made;
      if (far_moved) far_move_allocations += made;
    }
    last_layout = rig.tiles->layout();
    for (u32 l = 1; l < rig.tiles->level_count(); ++l) {
      most_chunks[l] = std::max<u32>(most_chunks[l], rig.tiles->chunks(l).size());
      CHECK(rig.tiles->chunks(l).size() * 2 <= rig.scene.terrain_slots(l));
    }
    if (f % 65 == 64) {
      CaptureChannels channels;
      channels.depth = true;
      CapturedFrame shot;
      REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
      CHECK(shot.covered == k_width * k_height);
    }
  }
  const TerrainMotion::RingStats& r = rig.motion.ring_stats();
  MESSAGE("a flight of " << 2 * k_frames << " m: " << r.swaps << " rebuilds swapped, "
                         << r.chunks_built << " tiles built and " << r.chunks_kept << " kept, "
                         << r.chunks_uploaded << " uploaded; most tiles a level held, coarsest "
                         << "first: " << most_chunks[1] << ", " << most_chunks[2] << ", "
                         << most_chunks[3] << ", " << most_chunks[4] << ", " << most_chunks[5]
                         << ", " << most_chunks[6] << "; the slots and arenas " << device_bytes
                         << " bytes throughout");
  CHECK(r.failed == 0);
  CHECK(r.swaps > 20);
  CHECK(rig.scene.terrain_ring_bytes() == device_bytes);
  CHECK(rig.tiles->withheld() == 0);
  CHECK(r.chunks_dropped > 0);
  CHECK(r.chunks_resident <= r.most_chunks);
  MESSAGE("the flight's second half: " << frame_buffers << " device buffers made on the frame's "
                                       << "thread; the staging ring held at most "
                                       << rig.scene.terrain_staging_peak() << " of its "
                                       << rig.scene.terrain_staging_bytes() << " bytes, "
                                       << rig.scene.terrain_staging_overflows() << " overflows");
  CHECK(frame_buffers == 0);
  CHECK(rig.scene.terrain_staging_overflows() == 0);
  if (mem::tracking_enabled()) {
    MESSAGE("the flight's second half: "
            << frame_allocations << " allocations on the frame's "
            << "thread over " << k_frames - k_frames / 2 << " frames, " << swaps_counted
            << " swaps and " << far_moves << " moves of a far level's square, "
            << far_move_allocations << " of them on the frames a far square moved");
    CHECK(swaps_counted > 10);
    CHECK(far_moves > 0);
    CHECK(frame_allocations == 0);
  }
}

// ---- the translation suite's terrain ----------------------------------------------------------
//
// **The world's tiles and their ground moved by whole cells with the camera draw the same bytes**
// (ADR-0053 decision 6; renderer.md, "The ground's tiles are placed at their corners"; the meshes'
// half of the suite is world_translation_tests.cpp). The dunes differ from place to place, so the
// ground here is a tile source of the test's own whose heights are a function of the lattice point
// **measured from its own origin**, in whole millimetres, which the test moves with the camera: by
// the origin and 6,548, 156,250 and 1,562,500 cells out (419 km, 10,000 km, 1e8 m), with and
// without far levels, on the mesh, software and both vertex rasterizers and the ray visibility
// pass. A tile is an instance at its corner and its vertices are metres from it, so a move by whole
// cells changes no operand a pass sees: ids, depth, the shading normal and the colour must be the
// same to the bit.
// Before 2026-10-05 a tile's vertices were world coordinates under an instance at cell zero, and
// 419 km out every one of them reached the frame through a float32 the size of the distance.
//
// **At the finest cut and at the default one** (2026-10-06). Until then only the finest held: a
// tile's UVs were in the scene grid's frame, so a tile 419 km out had UVs of thousands where its
// twin had fractions, and the DAG's simplification, which weighs UVs, built its coarser levels from
// other numbers — at the default cut about 22,400 id words, 10,100 depths, 18,900 normal bytes and
// 5,700 colour bytes of the 24,576 pixels differed on every path, at each of the three moves. A
// tile's UVs are from its corner now and its material adds the corner's place in the grid's frame
// (`gfx::terrain_uv_offset`), so its whole DAG is a function of its key and its heights.
namespace {

struct ShiftedGround {
  i64 x_mm = 0;  // where the ground's own origin is, whole millimetres from the world's
  i64 z_mm = 0;
};

bool shifted_heights(const void* state, f64, i64 spacing_mm, i64, i32 i0, i32 j0, u32 nx, u32 nz,
                     u32 begin, u32 end, std::span<f32> out) noexcept {
  const ShiftedGround& g = *static_cast<const ShiftedGround*>(state);
  if (out.size() != static_cast<usize>(nx) * nz) return false;
  const u32 per_side = (nx + scene_gen::k_height_block - 1) / scene_gen::k_height_block;
  for (u32 b = begin; b < end; ++b) {
    const u32 bx = (b % per_side) * scene_gen::k_height_block;
    const u32 bz = (b / per_side) * scene_gen::k_height_block;
    for (u32 z = bz; z < std::min(nz, bz + scene_gen::k_height_block); ++z) {
      for (u32 x = bx; x < std::min(nx, bx + scene_gen::k_height_block); ++x) {
        const f64 wx = static_cast<f64>((i0 + static_cast<i64>(x)) * spacing_mm - g.x_mm) / 1000.0;
        const f64 wz = static_cast<f64>((j0 + static_cast<i64>(z)) * spacing_mm - g.z_mm) / 1000.0;
        out[static_cast<usize>(z) * nx + x] =
            static_cast<f32>(1.5 * std::sin(0.31 * wx) * std::cos(0.23 * wz) + 0.05 * wx);
      }
    }
  }
  return true;
}
constexpr scene_gen::TileSourceOps k_shifted_ops{.heights = &shifted_heights};

}  // namespace

TEST_CASE(
    "world translation: the world's tiles moved by whole cells with their ground draw the same "
    "bytes") {
  {
    Gpu probe;
    if (!probe.ok) {
      MESSAGE("renderer unavailable here: " << probe.why);
      return;
    }
  }
  test::TempDir tmp{"engine_renderer_tiles_translation"};
  const SceneDesc desc = dune_scene(slashes(tmp.native() / "ddc"));
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  struct Path {
    const char* name;
    RasterMode raster;
    bool capacity;  // the vertex path without geometryShader: the capacity draw
  };
  const Path paths[] = {{"mesh", RasterMode::Hardware, false},
                        {"software", RasterMode::Software, false},
                        {"vertex indexed", RasterMode::Vertex, false},
                        {"vertex capacity", RasterMode::Vertex, true},
                        {"ray visibility", RasterMode::RayTrace, false}};
  struct Layout {
    const char* name;
    TerrainTilesDesc tiles;
  };
  const Layout layouts[] = {{"the world's rings", small_tiles()},
                            {"with far levels", small_far_tiles()}};
  const f64 shifts[] = {6548.0, 156250.0, 1562500.0};
  constexpr u32 k_width = 192;
  constexpr u32 k_height = 128;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  channels.normals = true;
  for (const Path& path : paths) {
    gfx::DeviceOptions options;
    if (path.capacity) {
      options.overrides.absent.push_back("VK_EXT_mesh_shader");
      options.overrides.absent.push_back("geometryShader");
    }
    gfx::Device device;
    std::string why;
    if (!device.create(options, &why)) {
      MESSAGE(std::string(path.name) << ": no device here: " << why);
      continue;
    }
    for (const Layout& layout : layouts) {
      for (const f32 cut : {0.0f, -1.0f}) {
        const char* cut_name = cut == 0.0f ? "the finest cut" : "the default cut";
        // The camera over the tiles by the origin, and the same camera and ground moved by `cells`
        // along x and -z.
        const auto picture = [&](f64 cells, CapturedFrame& out) -> bool {
          const i64 move_mm = static_cast<i64>(cells) * gfx::k_world_cell_mm;
          ShiftedGround ground{move_mm, -move_mm};
          const scene_gen::TileSource source{&k_shifted_ops, &ground};
          const DVec3 move{cells * k_world_cell_m, 0.0, -cells * k_world_cell_m};
          Camera camera;
          camera.position = WorldPos{3.25, 9.5, 14.0} + move;
          camera.target = WorldPos{-2.5, 0.0, -10.0} + move;
          camera.znear = 0.05f;
          RenderSettings settings;
          settings.raster = path.raster;
          settings.shadows = ShadowMode::Off;
          settings.lights = false;  // the stand-in point lights stand by the scene's grid
          TileRig rig;
          if (!rig.build(device, desc, settings, k_width, k_height, still_lapse(), &pool,
                         &layout.tiles, camera.position, &source)) {
            why = rig.error;
            return false;
          }
          rig.follow(camera);
          FrameDesc frame;
          frame.camera = camera;
          frame.lod_px = cut;
          if (!rig.renderer.capture(frame, channels, out, &rig.error)) {
            why = rig.error;
            return false;
          }
          return true;
        };
        CapturedFrame home;
        if (!picture(0.0, home)) {
          MESSAGE(std::string(path.name)
                  << ", " << std::string(layout.name) << ": not drawn here: " << why);
          break;
        }
        REQUIRE(home.covered > k_width * k_height / 2);
        for (const f64 shift : shifts) {
          CapturedFrame moved;
          REQUIRE_MESSAGE(picture(shift, moved), why);
          u64 ids = 0;
          u64 depth = 0;
          u64 normals = 0;
          u64 colour = 0;
          for (u32 i = 0; i < home.ids.size(); ++i)
            ids += home.ids[i] != moved.ids[i] ? 1u : 0u;
          for (u32 i = 0; i < home.depth.size(); ++i)
            depth += std::memcmp(&home.depth[i], &moved.depth[i], 4) != 0 ? 1u : 0u;
          for (u32 i = 0; i < home.normals.size(); ++i)
            normals += home.normals[i] != moved.normals[i] ? 1u : 0u;
          for (u32 i = 0; i < home.color.size(); ++i)
            colour += home.color[i] != moved.color[i] ? 1u : 0u;
          MESSAGE(std::string(path.name)
                  << ", " << std::string(layout.name) << ", " << std::string(cut_name)
                  << ", moved by " << shift << " cells: " << ids << " id words, " << depth
                  << " depths, " << normals << " normal bytes and " << colour
                  << " colour bytes differ");
          CHECK(moved.covered == home.covered);
          CHECK(ids == 0);
          CHECK(depth == 0);
          CHECK(normals == 0);
          CHECK(colour == 0);
        }
      }
    }
    device.destroy();
  }
}
