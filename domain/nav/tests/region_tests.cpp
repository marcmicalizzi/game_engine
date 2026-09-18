// The coarse tier: that it agrees with the detailed mesh about reachability, that its estimate
// is within a bounded factor of a real path, and that it stays in sync when tiles change.

#include "nav_test_support.h"

#include <domain/nav/region_graph.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::nav;
using namespace engine::nav::testing;

namespace {

// A 2 x 2 tile world, 128 m square, optionally cut in half by a wall along x = 64 with a gap of
// `gap` metres at the far end (z from 128 - gap to 128).
Soup world_with_divider(f32 gap) {
  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 32);
  if (gap < 128.0f) add_box(soup, Vec3(63.5f, 0.0f, 0.0f), Vec3(64.5f, 3.0f, 128.0f - gap));
  return soup;
}

struct World {
  NavBuildParams params = test_params();
  NavMesh mesh;
  RegionGraph graph;

  Status init() {
    NavMeshOptions options = test_mesh_options(params);
    options.max_tiles = 64;
    return mesh.init(options);
  }

  Status build_all(const Soup& soup) {
    for (i32 x = 0; x < 2; ++x) {
      for (i32 y = 0; y < 2; ++y) {
        const Status status = build_one(soup, TileCoord{x, y});
        if (status != Status::Ok) return status;
      }
    }
    return Status::Ok;
  }

  Status build_one(const Soup& soup, TileCoord coord) {
    NavTileData tile;
    const Status status = build_tile(params, coord, soup.view(), tile, nullptr);
    if (status != Status::Ok) return status;
    if (!tile.walkable()) {
      mesh.remove_tile(coord);
      graph.remove_tile(coord);
      return Status::Ok;
    }
    const Status added = mesh.add_tile(tile);
    if (added != Status::Ok) return added;
    graph.set_tile(tile);
    return Status::Ok;
  }

  f32 detailed(Vec3 from, Vec3 to) {
    Vec3 corridor[128];
    PathResult result;
    const Status status =
        mesh.find_path(from, to, walk_filter(), std::span<Vec3>(corridor), result);
    if (status != Status::Ok) return -1.0f;
    return result.length;
  }
};

}  // namespace

TEST_CASE("nav: the region graph has one node per tile of open floor, joined at the borders") {
  World world;
  REQUIRE(world.init() == Status::Ok);
  REQUIRE(world.build_all(world_with_divider(128.0f)) == Status::Ok);

  // Open floor: one region a tile, and every tile joined to the two it shares a border with.
  CHECK(world.graph.stats().tile_count == 4);
  CHECK(world.graph.node_count() == 4);
  CHECK(world.graph.edge_count() == 8);  // four pairs, counted once per direction

  const u32 corner = world.graph.find_region(Vec3(8.0f, 0.0f, 8.0f));
  REQUIRE(corner != k_invalid_region);
  const RegionNode* node = world.graph.node(corner);
  REQUIRE(node != nullptr);
  CHECK(node->coord == TileCoord{0, 0});
  CHECK(node->area > 3000.0f);
  CHECK(world.graph.neighbour_count(corner) == 2);

  // A point with no tile under it is in no region, which is how a caller tells "not loaded" from
  // "not reachable".
  CHECK(world.graph.find_region(Vec3(500.0f, 0.0f, 500.0f)) == k_invalid_region);
}

TEST_CASE("nav: the coarse estimate tracks the detailed path within a bounded factor") {
  World world;
  REQUIRE(world.init() == Status::Ok);
  // A divider with a 24 m gap at the far end: the only way across is round the end.
  REQUIRE(world.build_all(world_with_divider(24.0f)) == Status::Ok);

  struct Case {
    Vec3 from;
    Vec3 to;
  };
  const Case cases[4] = {
      {Vec3(8.0f, 0.5f, 8.0f), Vec3(120.0f, 0.5f, 8.0f)},      // across the divider, worst case
      {Vec3(8.0f, 0.5f, 8.0f), Vec3(8.0f, 0.5f, 120.0f)},      // straight up one side
      {Vec3(8.0f, 0.5f, 100.0f), Vec3(120.0f, 0.5f, 100.0f)},  // across, near the gap
      {Vec3(20.0f, 0.5f, 20.0f), Vec3(40.0f, 0.5f, 40.0f)},    // inside one region
  };

  for (const Case& c : cases) {
    const f32 detailed = world.detailed(c.from, c.to);
    REQUIRE(detailed > 0.0f);
    f32 estimate = 0.0f;
    REQUIRE(world.graph.estimate_distance(c.from, c.to, estimate));
    // The bound the docs page quotes. Measured on this scene the ratio is 1.00 to 1.09: the
    // estimate is portal-to-portal, so it can never cut through a wall (it is not much shorter
    // than the truth) and never detours via a centroid (it is not much longer). The asserted
    // band is wider than the measurement because the ratio is a property of the scene's region
    // shapes, and a long thin region would push it out without anything being wrong.
    CHECK(estimate >= detailed * 0.9f);
    CHECK(estimate <= detailed * 1.3f);
  }
}

TEST_CASE("nav: the coarse tier never claims a route the detailed mesh does not have") {
  World world;
  REQUIRE(world.init() == Status::Ok);
  REQUIRE(world.build_all(world_with_divider(0.0f)) == Status::Ok);

  const Vec3 west(8.0f, 0.5f, 64.0f);
  const Vec3 east(120.0f, 0.5f, 64.0f);
  CHECK_FALSE(world.graph.connected(west, east));

  Vec3 corridor[128];
  PathResult result;
  CHECK(world.mesh.find_path(west, east, walk_filter(), std::span<Vec3>(corridor), result) ==
        Status::Partial);

  // The same points on the same side are connected in both tiers.
  CHECK(world.graph.connected(west, Vec3(8.0f, 0.5f, 120.0f)));
}

TEST_CASE("nav: an off-mesh link joins two regions in the coarse tier too") {
  World world;
  REQUIRE(world.init() == Status::Ok);
  REQUIRE(world.build_all(world_with_divider(0.0f)) == Status::Ok);

  const Vec3 west(8.0f, 0.5f, 64.0f);
  const Vec3 east(120.0f, 0.5f, 64.0f);
  REQUIRE_FALSE(world.graph.connected(west, east));

  OffMeshLink link;
  link.start = Vec3(63.0f, 0.0f, 64.0f);
  link.end = Vec3(65.0f, 0.0f, 64.0f);
  link.radius = 1.0f;
  OffMeshLinkId id;
  REQUIRE(world.mesh.add_off_mesh_link(link, id) == Status::Ok);
  CHECK(world.graph.add_link(id.handle.to_u64(), link.start, link.end));

  CHECK(world.graph.connected(west, east));
  f32 estimate = 0.0f;
  REQUIRE(world.graph.estimate_distance(west, east, estimate));
  CHECK(estimate > 100.0f);
  CHECK(estimate < 180.0f);

  CHECK(world.graph.remove_link(id.handle.to_u64()));
  CHECK_FALSE(world.graph.connected(west, east));
}

TEST_CASE("nav: rebuilding a tile keeps the coarse tier in step with the detailed one") {
  World world;
  REQUIRE(world.init() == Status::Ok);
  REQUIRE(world.build_all(world_with_divider(24.0f)) == Status::Ok);

  const u32 nodes_before = world.graph.node_count();
  const u64 revision_before = world.graph.stats().revision;

  // The divider is destroyed. Both halves of the world become one region per tile again.
  const Soup open = world_with_divider(128.0f);
  for (i32 x = 0; x < 2; ++x) {
    for (i32 y = 0; y < 2; ++y)
      REQUIRE(world.build_one(open, TileCoord{x, y}) == Status::Ok);
  }
  CHECK(world.graph.stats().revision > revision_before);
  CHECK(world.graph.node_count() == 4);
  CHECK(world.graph.node_count() <= nodes_before);

  f32 estimate = 0.0f;
  REQUIRE(
      world.graph.estimate_distance(Vec3(8.0f, 0.5f, 8.0f), Vec3(120.0f, 0.5f, 8.0f), estimate));
  CHECK(estimate < 160.0f);  // straight across now, not round the end

  // Removing a tile takes its nodes with it and leaves nothing dangling.
  world.mesh.remove_tile(TileCoord{1, 1});
  world.graph.remove_tile(TileCoord{1, 1});
  CHECK(world.graph.node_count() == 3);
  CHECK(world.graph.edge_count() == 4);
  CHECK(world.graph.find_region(Vec3(120.0f, 0.5f, 120.0f)) == k_invalid_region);

  world.graph.clear();
  CHECK(world.graph.node_count() == 0);
  CHECK(world.graph.edge_count() == 0);
}

TEST_CASE("nav: the coarse tier can be fed from the bytes the mesh already holds") {
  World world;
  REQUIRE(world.init() == Status::Ok);
  REQUIRE(world.build_all(world_with_divider(128.0f)) == Status::Ok);

  RegionGraph rebuilt;
  for (i32 x = 0; x < 2; ++x) {
    for (i32 y = 0; y < 2; ++y)
      CHECK(rebuilt.set_tile_bytes(world.mesh.tile_bytes(TileCoord{x, y})));
  }
  CHECK(rebuilt.node_count() == world.graph.node_count());
  CHECK(rebuilt.edge_count() == world.graph.edge_count());
}
