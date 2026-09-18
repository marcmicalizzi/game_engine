// E11's first data point (plan 10 §10.5): what a Recast tile rebuild costs on modern hardware at
// the tile size we would actually choose, and what the rebuild queue sustains on eight workers.
//
// The scene is the one the experiment asks for: a heightfield on a 1 m grid with scattered boxes
// on it, which is the shape of a destructible town — a floor whose triangle count grows with the
// tile's area, plus obstacles that make the region builder work rather than producing one
// rectangle. Rebuild cost is quadratic in the tile's side at a fixed cell size, so the three tile
// sizes are the measurement that decides the nav rebuild budget.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/nav/nav_mesh.h>
#include <domain/nav/rebuild_queue.h>
#include <domain/nav/region_graph.h>
#include <domain/nav/tile.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::nav;

namespace {

struct Soup {
  Vector<Vec3> vertices;
  Vector<u32> indices;
};

void add_quad(Soup& soup, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
  const u32 base = soup.vertices.size();
  soup.vertices.push_back(a);
  soup.vertices.push_back(b);
  soup.vertices.push_back(c);
  soup.vertices.push_back(d);
  const u32 order[6] = {0, 1, 2, 0, 2, 3};
  for (const u32 i : order)
    soup.indices.push_back(base + i);
}

void add_box(Soup& soup, Vec3 lo, Vec3 hi) {
  const u32 base = soup.vertices.size();
  soup.vertices.push_back(Vec3(lo.x, lo.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, lo.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, lo.y, hi.z));
  soup.vertices.push_back(Vec3(lo.x, lo.y, hi.z));
  soup.vertices.push_back(Vec3(lo.x, hi.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, hi.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, hi.y, hi.z));
  soup.vertices.push_back(Vec3(lo.x, hi.y, hi.z));
  const u32 faces[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                         1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  for (const u32 i : faces)
    soup.indices.push_back(base + i);
}

// A deterministic, cheap hash to place the boxes: the same scene every run, with no dependence on
// a random engine's implementation.
u32 mix(u32 x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// A 1 m-cell heightfield covering [0, side] x [0, side], gently undulating so the rasterizer has
// real spans to merge, with a box on roughly one cell in twenty.
Soup make_scene(f32 side) {
  Soup soup;
  const u32 cells = static_cast<u32>(side);
  auto height = [](u32 x, u32 z) { return 0.05f * static_cast<f32>((x * 7u + z * 13u) % 5u); };
  for (u32 x = 0; x < cells; ++x) {
    for (u32 z = 0; z < cells; ++z) {
      const f32 x0 = static_cast<f32>(x);
      const f32 z0 = static_cast<f32>(z);
      add_quad(soup, Vec3(x0, height(x, z), z0), Vec3(x0, height(x, z + 1), z0 + 1.0f),
               Vec3(x0 + 1.0f, height(x + 1, z + 1), z0 + 1.0f),
               Vec3(x0 + 1.0f, height(x + 1, z), z0));
      if (mix(x * 73856093u ^ z * 19349663u) % 20u == 0u) {
        const f32 w = 0.6f + static_cast<f32>(mix(x + z * 31u) % 4u) * 0.4f;
        add_box(soup, Vec3(x0 + 0.2f, 0.0f, z0 + 0.2f), Vec3(x0 + 0.2f + w, 2.5f, z0 + 0.2f + w));
      }
    }
  }
  return soup;
}

NavBuildParams bench_params(f32 tile_size) {
  NavBuildParams params;
  params.cell_size = 0.25f;  // divides 32, 64 and 128 exactly, which validate() requires
  params.cell_height = 0.2f;
  params.tile_size = tile_size;
  params.origin = Vec3(0.0f, 0.0f, 0.0f);
  params.agent_radius = 0.5f;
  params.agent_height = 2.0f;
  params.agent_max_climb = 0.4f;
  params.height_min = -8.0f;
  params.height_max = 32.0f;
  return params;
}

TileGeometry view_of(const Soup& soup) {
  TileGeometry geometry;
  geometry.vertices = std::span<const Vec3>(soup.vertices.data(), soup.vertices.size());
  geometry.indices = std::span<const u32>(soup.indices.data(), soup.indices.size());
  return geometry;
}

}  // namespace

// One tile, single-threaded, at three tile sizes. The geometry handed over is exactly the tile's
// own square, so the number is the pipeline's and not the caller's triangle rejection.
ENGINE_BENCH_ARGS(nav_build_tile, "nav.build.tile", 32, 64, 128) {
  const f32 side = static_cast<f32>(state.arg());
  const NavBuildParams params = bench_params(side);
  const Soup soup = make_scene(side);
  const TileGeometry geometry = view_of(soup);

  NavTileData tile;
  TileBuildStats stats;
  build_tile(params, TileCoord{0, 0}, geometry, tile, &stats);
  bench::keep(stats.poly_count);

  while (state.keep_running()) {
    NavTileData built;
    const Status status = build_tile(params, TileCoord{0, 0}, geometry, built, nullptr);
    bench::keep(status);
    bench::keep(built.poly_count());
  }
  state.set_items(1);
  state.set_bytes(tile.bytes().size());
}

// The same tiles at the settings a game would drop to when the rebuild budget is tight: half the
// voxel resolution, monotone partitioning instead of watershed, and no detail mesh. The point of
// the pair is that E11's "nav rebuild budget" is not one number — it is a knob with a factor of
// several on it — and this is the cheap end of it.
ENGINE_BENCH_ARGS(nav_build_tile_coarse, "nav.build.tile_coarse", 32, 64, 128) {
  const f32 side = static_cast<f32>(state.arg());
  NavBuildParams params = bench_params(side);
  params.cell_size = 0.5f;  // still divides 32, 64 and 128 exactly
  params.cell_height = 0.3f;
  params.monotone_regions = true;
  params.detail_sample_dist = 0.0f;  // below 0.9 turns the detail mesh's sampling off
  const Soup soup = make_scene(side);
  const TileGeometry geometry = view_of(soup);

  while (state.keep_running()) {
    NavTileData built;
    const Status status = build_tile(params, TileCoord{0, 0}, geometry, built, nullptr);
    bench::keep(status);
    bench::keep(built.poly_count());
  }
  state.set_items(1);
}

// What the queue sustains: sixteen 64 m tiles of the same scene, dispatched together, at one,
// four, and eight efficiency workers. The rate column is tiles per second.
ENGINE_BENCH_ARGS(nav_queue_tiles, "nav.queue.tiles", 1, 4, 8) {
  const u32 workers = static_cast<u32>(state.arg());
  constexpr u32 k_side = 4;  // a 4 x 4 block of tiles
  constexpr u32 k_tiles = k_side * k_side;
  constexpr f32 k_tile_size = 64.0f;

  jobs::JobSystemConfig config;
  config.performance_workers = 1;
  config.efficiency_workers = workers;
  jobs::JobSystem job_system(config);

  const NavBuildParams params = bench_params(k_tile_size);
  const Soup soup = make_scene(k_tile_size * static_cast<f32>(k_side));
  const TileGeometry geometry = view_of(soup);

  RebuildQueueOptions options;
  options.build = params;
  options.job_system = &job_system;
  options.max_in_flight = workers;
  options.max_pending = 64;
  RebuildQueue queue;
  queue.init(options);

  NavMesh mesh;
  NavMeshOptions mesh_options;
  mesh_options.origin = params.origin;
  mesh_options.tile_size = k_tile_size;
  mesh_options.max_tiles = 64;
  mesh.init(mesh_options);
  RegionGraph graph;

  while (state.keep_running()) {
    for (u32 i = 0; i < k_tiles; ++i) {
      RebuildRequest request;
      request.coord = TileCoord{static_cast<i32>(i % k_side), static_cast<i32>(i / k_side)};
      request.vertices = geometry.vertices;
      request.indices = geometry.indices;
      queue.request(request);
    }
    queue.wait_idle();
    const u32 applied = queue.apply_ready(mesh, &graph);
    bench::keep(applied);
  }
  state.set_items(k_tiles);
  bench::keep(mesh.tile_count());
  bench::keep(graph.node_count());
}
