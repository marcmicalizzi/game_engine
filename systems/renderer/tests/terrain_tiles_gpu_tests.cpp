// The ground from the world's tiles, on a GPU (docs/subsystems/renderer.md, "The ground from the
// world's tiles"; ADR-0048), held by the visibility buffer and not by eye:
//
//   - the erg drawn from tiles at the grid's spacing is the fixed grid's picture inside the grid's
//     extent, pixel for pixel, at the finest cut — colour and depth — and has ground past it;
//   - tiles meet with no crack: every pixel of a view that is all ground is covered, no column of
//     a grazing view has a hole under its ground, and every covered pixel stands on the surface —
//     at one level, across levels, a time-lapse step apart, and 50 km from the origin;
//   - no lighting seam: the shading normal steps across a tile's border no more than it does
//     between two pixels inside a tile;
//   - the time-lapse holds across tiles: one surface time, each level within its bound;
//   - a tile set built ahead and read back from a derived-data cache draws the procedural one's
//     picture, pixel for pixel: the renderer does not know who made a tile;
//   - a long flight keeps the resident tiles bounded and never drops a rebuild.
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
             const TerrainTilesDesc* tile_desc, Vec3 camera,
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
      if (!tiles->build(data.terrain, *tile_desc, src, camera.x, camera.z, jobs, &error))
        return false;
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
      terrain_tiles_round(tiles->tiles_desc(), camera.position.x, camera.position.z, held);
      tiles->set_tiles(std::span<const TerrainTile>(held.data(), held.size()));
    }
    motion.frame(dt, camera.position.x, camera.position.z);
  }
};

TimeLapseConfig still_lapse() {
  TimeLapseConfig lapse;
  lapse.rate = 0.0;
  lapse.wait = true;
  return lapse;
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

Census census(const CapturedFrame& shot, const Mat4& inverse_view_proj, const TerrainTileSet& set,
              const SurfaceModel* model, u32 model_stride = 7) {
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
      if (!unproject(shot, inverse_view_proj, px, py, world[p])) {
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
      if (set.find(tx, tz, l, index)) level[p] = static_cast<i32>(l);
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
  // Every covered pixel on the surface, within the relief its fans may span (a sample of them).
  if (model != nullptr) {
    for (u32 p = 0; p < u64{w} * h; p += model_stride) {
      if (has[p] == 0) continue;
      f32 lo = 0.0f;
      f32 hi = 0.0f;
      model->bounds(world[p].x, world[p].z, lo, hi);
      const f32 y = world[p].y;
      const f64 off = y < lo ? lo - y : y > hi ? y - hi : 0.0;
      if (off > 0.005) ++c.off_surface;
      c.worst_off_m = std::max(c.worst_off_m, off);
    }
  }
  // The normal's step between each pixel and its right and lower neighbours, by what lies between,
  // and its excess over the steps on either side of it along the same line.
  const auto step_of = [&](u32 p, u32 q, f32& out) {
    if (has[p] == 0 || has[q] == 0) return false;
    // Neighbours a pixel apart on the ground, not across a silhouette.
    if (length(world[p] - world[q]) > 0.25f) return false;
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

void check_seams(const Census& c, const std::string& what) {
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
  constexpr f32 k_quantum = 0.016f;
  CHECK(percentile(c.same_level_steps, 1.0) <= percentile(c.inside_steps, 1.0) + k_quantum);
  CHECK(percentile(c.cross_level_steps, 1.0) <= percentile(c.inside_steps, 1.0) + k_quantum);
  CHECK(percentile(c.same_level_excess, 1.0) <= percentile(c.inside_excess, 1.0) + k_quantum);
  CHECK(percentile(c.cross_level_excess, 1.0) <= percentile(c.inside_excess, 1.0) + k_quantum);
}

// A view of the ground from above that straddles every level's borders round `centre`.
Camera looking_down_at(Vec3 centre, f32 height = 26.0f) {
  Camera camera;
  camera.position = Vec3{centre.x - 3.0f, height, centre.z + 9.0f};
  camera.target = Vec3{centre.x + 1.0f, 0.0f, centre.z - 2.0f};
  camera.znear = 0.5f;
  return camera;
}
// A grazing one, a couple of metres over the sand, looking out over the rings.
Camera looking_across(Vec3 eye_ground, f32 ground_y) {
  Camera camera;
  camera.position = Vec3{eye_ground.x, ground_y + 2.5f, eye_ground.z};
  camera.target = Vec3{eye_ground.x + 30.0f, ground_y - 4.0f, eye_ground.z - 18.0f};
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
      Camera{Vec3{-40.0f, 90.0f, 60.0f}, Vec3{30.0f, 0.0f, -40.0f}},
      Camera{Vec3{120.0f, 40.0f, -150.0f}, Vec3{40.0f, 10.0f, -60.0f}},
      Camera{Vec3{0.0f, 250.0f, 0.1f}, Vec3{0.0f, 0.0f, 0.0f}},
  };
  TileRig grid;
  REQUIRE_MESSAGE(grid.build(gpu.device, erg, settings, k_width, k_height, still_lapse(), &pool,
                             nullptr, Vec3{}),
                  grid.error);
  TileRig tiles;
  REQUIRE_MESSAGE(
      tiles.build(gpu.device, erg, settings, k_width, k_height, still_lapse(), &pool, &t, Vec3{}),
      tiles.error);
  CaptureChannels channels;
  channels.depth = true;
  channels.ids = true;
  u32 compared = 0;
  u32 depth_differ = 0;
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
        const bool in_grid = unproject(a, inverse_view_proj, px, py, world);
        if (!in_grid) {
          beyond += b.depth[p] > 0.0f ? 1u : 0u;  // ground the tiles have past the grid
          continue;
        }
        // Inside the grid, two samples from its edge, where its normals are one-sided.
        if (std::abs(world.x) > 189.0f || std::abs(world.z) > 189.0f) continue;
        ++compared;
        const bool depth_off = a.depth[p] != b.depth[p];
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
                               << " differ in depth, " << color_differ << " in colour; " << beyond
                               << " pixels of ground past the grid's edge");
  CHECK(compared > 50'000);
  CHECK(depth_differ == 0);
  CHECK(color_differ == 0);
  CHECK(beyond > 0);

  // And the desert goes on: two kilometres past the grid's edge, every pixel of a view down on the
  // sand is ground.
  const Camera far{Vec3{2'200.0f, 120.0f, -1'700.0f}, Vec3{2'230.0f, 0.0f, -1'760.0f}};
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
    REQUIRE_MESSAGE(
        rig.build(gpu.device, desc, settings, k_width, k_height, still_lapse(), &pool, &t, centre),
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
      for (const f32 lod : {0.0f, 1.0f}) {  // the finest cut, and the default's coarser one
        frame.lod_px = lod;
        CapturedFrame shot;
        REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
        const Census c = census(shot, inverse(rig.renderer.views()[0].view_proj), *rig.tiles,
                                lod == 0.0f ? &model : nullptr);
        const std::string what = std::string(origin_x > 0.0f ? "50 km out, " : "at the origin, ") +
                                 (grazing ? "grazing" : "from above") +
                                 (lod == 0.0f ? ", the finest cut" : ", a pixel's cut");
        if (!grazing) CHECK(c.uncovered == 0);
        check_seams(c, what);
      }
    }
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
  const TerrainTilesDesc t = small_tiles();
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
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height, lapse, &pool, &t, start),
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
    const Census c = census(shot, inverse(rig.renderer.views()[0].view_proj), *rig.tiles, nullptr);
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

  u64 key_of(i32 x, i32 z) const noexcept {
    return hash_combine(
        hash_combine(name, static_cast<u64>(static_cast<u32>(x))),
        hash_combine(static_cast<u64>(static_cast<u32>(z)), static_cast<u64>(spacing_mm)));
  }
  std::string path_of(u64 key) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
    return ddc + "/terrain_tiles/" + hex + ".tile";
  }
  const Vector<f32>* tile(i32 x, i32 z) const {
    const u64 key = key_of(x, z);
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
  static bool heights(const void* state, f64, i64 spacing_mm, i32 i0, i32 j0, u32 nx, u32 nz,
                      u32 begin, u32 end, std::span<f32> out) noexcept {
    const auto& self = *static_cast<const BuiltTileSet*>(state);
    if (spacing_mm % self.spacing_mm != 0 || out.size() != static_cast<usize>(nx) * nz)
      return false;
    const i64 r = spacing_mm / self.spacing_mm;
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
          const Vector<f32>* h = self.tile(tx, tz);
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
  const TerrainTilesDesc t = small_tiles();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 256;
  constexpr u32 k_height = 192;
  const Vec3 centre{3.0f, 0.0f, -2.0f};
  // The procedural picture.
  TileRig procedural;
  REQUIRE_MESSAGE(procedural.build(gpu.device, desc, settings, k_width, k_height, still_lapse(),
                                   &pool, &t, centre),
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
  for (i32 x = -9; x <= 9; ++x) {
    for (i32 z = -9; z <= 9; ++z) {
      const u32 a = set.cells + 3;
      Vector<f32> h(u64{a} * a);
      REQUIRE(ground.heights(desc.terrain.time_s, set.spacing_mm, x * 16 - 1, z * 16 - 1, a, a,
                             std::span<f32>(h.data(), h.size())));
      const std::string_view bytes(reinterpret_cast<const char*>(h.data()), h.size() * sizeof(f32));
      REQUIRE(io::write_file(set.path_of(set.key_of(x, z)), bytes) == io::Status::Ok);
      ++written;
    }
  }
  // Read back through the same seam: the renderer is handed a source and nothing else.
  const scene_gen::TileSource built{&k_built_ops, &set};
  TileRig authored;
  REQUIRE_MESSAGE(authored.build(gpu.device, desc, settings, k_width, k_height, still_lapse(),
                                 &pool, &t, centre, &built),
                  authored.error);
  CaptureChannels channels;
  channels.depth = true;
  u32 compared = 0;
  u32 differ = 0;
  for (const bool grazing : {false, true}) {
    const Camera camera =
        grazing ? looking_across(centre, procedural.ground->height(centre.x, centre.z))
                : looking_down_at(centre);
    procedural.follow(camera);
    authored.follow(camera);
    FrameDesc frame;
    frame.camera = camera;
    CapturedFrame a;
    CapturedFrame b;
    REQUIRE_MESSAGE(procedural.renderer.capture(frame, channels, a, &procedural.error),
                    procedural.error);
    REQUIRE_MESSAGE(authored.renderer.capture(frame, channels, b, &authored.error), authored.error);
    for (u32 p = 0; p < k_width * k_height; ++p) {
      if (!(a.depth[p] > 0.0f)) continue;
      ++compared;
      bool same = a.depth[p] == b.depth[p];
      for (u32 k = 0; k < 4 && same; ++k)
        same = a.color[u64{p} * 4 + k] == b.color[u64{p} * 4 + k];
      differ += same ? 0u : 1u;
    }
  }
  MESSAGE("a tile set built ahead: " << written << " tiles written, " << set.reads << " read back, "
                                     << compared << " pixels compared, " << differ << " differ");
  CHECK(set.reads > 0);
  CHECK(compared > 40'000);
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
  const TerrainTilesDesc t = small_tiles();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 160;
  constexpr u32 k_height = 120;
  TileRig rig;
  REQUIRE_MESSAGE(
      rig.build(gpu.device, desc, settings, k_width, k_height, still_lapse(), &pool, &t, Vec3{}),
      rig.error);
  const u64 device_bytes = rig.scene.terrain_ring_bytes();
  u32 most_chunks[k_max_terrain_levels] = {};
  // **The frame loop allocates nothing in steady state**: the host's half of a frame — the tiles
  // handed over, the motion's frame with its re-centres asked for, taken, uploaded and swapped — is
  // counted on this thread alone (a tag is the calling thread's, so the workers' builds are not),
  // over the flight's second half, once every list has reached the size the flight needs.
  static const mem::TagId k_frames_tag = mem::register_tag("tile-flight-frames");
  u64 frame_allocations = 0;
  u32 swaps_counted = 0;
  // 60 tiles east and 20 north, two metres a frame: every tile the flight passes comes and goes.
  constexpr u32 k_frames = 260;
  for (u32 f = 0; f < k_frames; ++f) {
    const f32 x = 2.0f * static_cast<f32>(f);
    const f32 z = 0.6f * static_cast<f32>(f);
    const u64 allocations_before = mem::stats(k_frames_tag).allocation_count;
    const u32 swaps_before = rig.motion.ring_stats().swaps;
    {
      const mem::TagScope scope(k_frames_tag);
      rig.follow(looking_down_at(Vec3{x, 0.0f, z}, 30.0f));
    }
    if (f >= k_frames / 2) {
      frame_allocations += mem::stats(k_frames_tag).allocation_count - allocations_before;
      swaps_counted += rig.motion.ring_stats().swaps - swaps_before;
    }
    for (u32 l = 1; l < rig.tiles->level_count(); ++l) {
      most_chunks[l] = std::max<u32>(most_chunks[l], rig.tiles->chunks(l).size());
      CHECK(rig.tiles->chunks(l).size() * 2 <= rig.scene.terrain_slots(l));
    }
    if (f % 65 == 64) {
      FrameDesc frame;
      frame.camera = looking_down_at(Vec3{x, 0.0f, z}, 30.0f);
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
                         << r.chunks_uploaded << " uploaded; most tiles a level held: "
                         << most_chunks[1] << ", " << most_chunks[2] << ", " << most_chunks[3]
                         << "; the slots and arenas " << device_bytes << " bytes throughout");
  CHECK(r.failed == 0);
  CHECK(r.swaps > 20);
  CHECK(rig.scene.terrain_ring_bytes() == device_bytes);
  CHECK(rig.tiles->withheld() == 0);
  CHECK(r.chunks_dropped > 0);
  CHECK(r.chunks_resident <= r.most_chunks);
  if (mem::tracking_enabled()) {
    MESSAGE("the flight's second half: " << frame_allocations << " allocations on the frame's "
                                         << "thread over " << k_frames - k_frames / 2
                                         << " frames and " << swaps_counted << " swaps");
    CHECK(swaps_counted > 10);
    CHECK(frame_allocations == 0);
  }
}
