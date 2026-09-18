// Paths: around a wall, straight once the wall is gone, and shortened by an off-mesh link.

#include "nav_test_support.h"

#include <core/containers/vector.h>
#include <domain/nav/nav_mesh.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::nav;
using namespace engine::nav::testing;

namespace {

// The two ends of the crossing every path test uses: opposite sides of the wall at x = 32.
constexpr Vec3 k_west(8.0f, 0.5f, 24.0f);
constexpr Vec3 k_east(56.0f, 0.5f, 24.0f);

struct Fixture {
  NavBuildParams params = test_params();
  NavMesh mesh;

  Status build(f32 wall_length) {
    const Soup soup = floor_with_wall(wall_length);
    NavTileData tile;
    const Status status = build_tile(params, TileCoord{0, 0}, soup.view(), tile, nullptr);
    if (status != Status::Ok) return status;
    return mesh.add_tile(tile);
  }
};

f32 path_between(const NavMesh& mesh, Vec3 from, Vec3 to, PathResult& out) {
  Vec3 corridor[64];
  const Status status = mesh.find_path(from, to, walk_filter(), std::span<Vec3>(corridor), out);
  CHECK(status == Status::Ok);
  return out.length;
}

}  // namespace

TEST_CASE("nav: a path goes around a wall, and is straight once the wall is rebuilt away") {
  Fixture fixture;
  REQUIRE(fixture.mesh.init(test_mesh_options(fixture.params)) == Status::Ok);

  // A wall from z = 0 to z = 48 leaves a 16 m gap at the far end.
  REQUIRE(fixture.build(48.0f) == Status::Ok);
  PathResult around;
  const f32 around_length = path_between(fixture.mesh, k_west, k_east, around);
  CHECK_FALSE(around.partial);
  CHECK(around.count > 2);  // a straight walk would be two points
  // Straight across is 48 m; going round the end of the wall at z = 48 is two legs of about
  // 34.6 m, so a little under 70.
  CHECK(around_length > 65.0f);

  // The wall is destroyed and the tile rebuilt. The same query is now a straight line.
  const u64 before = fixture.mesh.revision();
  REQUIRE(fixture.build(0.0f) == Status::Ok);
  CHECK(fixture.mesh.revision() > before);
  CHECK(fixture.mesh.tile_count() == 1);  // replaced, not added beside

  PathResult straight;
  const f32 straight_length = path_between(fixture.mesh, k_west, k_east, straight);
  CHECK_FALSE(straight.partial);
  CHECK(straight.count == 2);
  CHECK(straight_length == doctest::Approx(48.0f).epsilon(0.02));
  CHECK(straight_length < around_length * 0.7f);
}

TEST_CASE("nav: a wall all the way across makes the goal unreachable, and says so") {
  Fixture fixture;
  REQUIRE(fixture.mesh.init(test_mesh_options(fixture.params)) == Status::Ok);
  REQUIRE(fixture.build(64.0f) == Status::Ok);

  Vec3 corridor[64];
  PathResult result;
  const Status status =
      fixture.mesh.find_path(k_west, k_east, walk_filter(), std::span<Vec3>(corridor), result);
  CHECK(status == Status::Partial);
  CHECK(result.partial);
  CHECK(result.count >= 1);
  // The corridor ends against the wall, on the near side of it.
  CHECK(corridor[result.count - 1].x < 32.0f);
}

TEST_CASE("nav: an off-mesh link across a gap shortens the path") {
  Fixture fixture;
  REQUIRE(fixture.mesh.init(test_mesh_options(fixture.params)) == Status::Ok);
  REQUIRE(fixture.build(48.0f) == Status::Ok);

  PathResult before;
  const f32 before_length = path_between(fixture.mesh, k_west, k_east, before);

  // A hole opens in the wall at z = 24: the two sides are now a step apart.
  OffMeshLink link;
  link.start = Vec3(31.0f, 0.0f, 24.0f);
  link.end = Vec3(33.0f, 0.0f, 24.0f);
  link.radius = 1.0f;
  link.flags = k_flag_jump;
  link.area = Area::Jump;
  link.bidirectional = true;
  OffMeshLinkId id;
  REQUIRE(fixture.mesh.add_off_mesh_link(link, id) == Status::Ok);
  CHECK(fixture.mesh.off_mesh_link_count() == 1);

  PathFilter filter;
  filter.include_flags = static_cast<u16>(k_flag_walk | k_flag_jump);
  Vec3 corridor[64];
  PathResult after;
  REQUIRE(fixture.mesh.find_path(k_west, k_east, filter, std::span<Vec3>(corridor), after) ==
          Status::Ok);
  CHECK_FALSE(after.partial);
  CHECK(after.length < before_length * 0.8f);
  CHECK(after.length == doctest::Approx(48.0f).epsilon(0.1));

  // Removing the link puts the long way back.
  CHECK(fixture.mesh.remove_off_mesh_link(id));
  CHECK(fixture.mesh.off_mesh_link_count() == 0);
  PathResult restored;
  REQUIRE(fixture.mesh.find_path(k_west, k_east, filter, std::span<Vec3>(corridor), restored) ==
          Status::Ok);
  CHECK(restored.length == doctest::Approx(before_length).epsilon(0.02));

  // And a filter that does not include jumping never uses the link. The default filter does
  // include it — it admits every flag but `disabled` — so this needs an explicit walk-only one.
  REQUIRE(fixture.mesh.add_off_mesh_link(link, id) == Status::Ok);
  PathResult walker;
  REQUIRE(fixture.mesh.find_path(k_west, k_east, walk_only_filter(), std::span<Vec3>(corridor),
                                 walker) == Status::Ok);
  CHECK(walker.length == doctest::Approx(before_length).epsilon(0.02));
}

TEST_CASE("nav: a link added before its tile is baked in when the tile arrives") {
  Fixture fixture;
  REQUIRE(fixture.mesh.init(test_mesh_options(fixture.params)) == Status::Ok);

  OffMeshLink link;
  link.start = Vec3(31.0f, 0.0f, 24.0f);
  link.end = Vec3(33.0f, 0.0f, 24.0f);
  link.radius = 1.0f;
  OffMeshLinkId id;
  // No tiles at all yet: the link is stored, not refused, because destruction opening a passage
  // into a room whose tile has not been rebuilt is a normal race.
  REQUIRE(fixture.mesh.add_off_mesh_link(link, id) == Status::Ok);
  REQUIRE(fixture.build(48.0f) == Status::Ok);

  PathFilter filter;
  filter.include_flags = static_cast<u16>(k_flag_walk | k_flag_jump);
  Vec3 corridor[64];
  PathResult result;
  REQUIRE(fixture.mesh.find_path(k_west, k_east, filter, std::span<Vec3>(corridor), result) ==
          Status::Ok);
  CHECK(result.length == doctest::Approx(48.0f).epsilon(0.1));
}

TEST_CASE("nav: find_nearest snaps to the mesh and raycast stops at a wall") {
  Fixture fixture;
  REQUIRE(fixture.mesh.init(test_mesh_options(fixture.params)) == Status::Ok);
  REQUIRE(fixture.build(48.0f) == Status::Ok);

  NavPoint point;
  REQUIRE(fixture.mesh.find_nearest(Vec3(16.0f, 1.5f, 16.0f), walk_filter(), point));
  CHECK(point.valid());
  CHECK(point.position.y == doctest::Approx(0.0f).epsilon(0.2));
  CHECK(point.position.x == doctest::Approx(16.0f).epsilon(0.05));

  // Nothing within the search box, well outside the mesh.
  CHECK_FALSE(fixture.mesh.find_nearest(Vec3(200.0f, 0.0f, 200.0f), walk_filter(), point));

  // A ray straight at the wall stops at it; a ray along the open floor does not.
  RaycastHit hit;
  REQUIRE(fixture.mesh.raycast(k_west, k_east, walk_filter(), hit));
  CHECK(hit.hit);
  CHECK(hit.position.x < 32.0f);
  CHECK(hit.position.x > 28.0f);
  CHECK(hit.fraction < 1.0f);

  RaycastHit clear;
  REQUIRE(
      fixture.mesh.raycast(Vec3(8.0f, 0.5f, 8.0f), Vec3(8.0f, 0.5f, 56.0f), walk_filter(), clear));
  CHECK_FALSE(clear.hit);
  CHECK(clear.fraction == doctest::Approx(1.0f));
}

TEST_CASE("nav: a corridor that does not fit reports what did") {
  Fixture fixture;
  REQUIRE(fixture.mesh.init(test_mesh_options(fixture.params)) == Status::Ok);
  REQUIRE(fixture.build(48.0f) == Status::Ok);

  Vec3 tiny[2];
  PathResult result;
  const Status status =
      fixture.mesh.find_path(k_west, k_east, walk_filter(), std::span<Vec3>(tiny), result);
  CHECK(status == Status::LimitReached);
  CHECK(result.count == 2);

  Vec3 one[1];
  CHECK(fixture.mesh.find_path(k_west, k_east, walk_filter(), std::span<Vec3>(one), result) ==
        Status::InvalidArgument);
}

TEST_CASE("nav: tiles must be built on the mesh's own grid") {
  Fixture fixture;
  NavMeshOptions options = test_mesh_options(fixture.params);
  options.tile_size = 32.0f;  // the mesh's grid, not the builder's
  REQUIRE(fixture.mesh.init(options) == Status::Ok);

  const Soup soup = floor_with_wall(0.0f);
  NavTileData tile;
  REQUIRE(build_tile(fixture.params, TileCoord{0, 0}, soup.view(), tile, nullptr) == Status::Ok);
  CHECK(fixture.mesh.add_tile(tile) == Status::InvalidArgument);
  CHECK(fixture.mesh.tile_count() == 0);
}

TEST_CASE("nav: the tile and polygon caps have to fit a 32-bit polygon reference") {
  NavMesh mesh;
  NavMeshOptions options;
  options.max_tiles = 8192;           // 13 bits
  options.max_polys_per_tile = 4096;  // 12 bits, and 25 > 22
  CHECK(mesh.init(options) == Status::LimitReached);

  options.max_tiles = 1024;
  CHECK(mesh.init(options) == Status::Ok);
}

TEST_CASE("nav: a path across four tiles crosses their borders") {
  // The test that catches the seam bug: two tiles built separately only link if their voxel
  // grids meet exactly on the shared border.
  NavBuildParams params = test_params();
  NavMesh mesh;
  REQUIRE(mesh.init(test_mesh_options(params)) == Status::Ok);

  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 32);
  for (i32 x = 0; x < 2; ++x) {
    for (i32 y = 0; y < 2; ++y) {
      NavTileData tile;
      REQUIRE(build_tile(params, TileCoord{x, y}, soup.view(), tile, nullptr) == Status::Ok);
      REQUIRE(tile.walkable());
      REQUIRE(mesh.add_tile(tile) == Status::Ok);
    }
  }
  CHECK(mesh.tile_count() == 4);

  Vec3 corridor[64];
  PathResult result;
  REQUIRE(mesh.find_path(Vec3(8.0f, 0.5f, 8.0f), Vec3(120.0f, 0.5f, 120.0f), walk_filter(),
                         std::span<Vec3>(corridor), result) == Status::Ok);
  CHECK_FALSE(result.partial);
  // Straight across the diagonal of a 128 m square, with nothing in the way.
  CHECK(result.length == doctest::Approx(158.4f).epsilon(0.03));

  // Removing the far tile makes the same query partial rather than silently wrong.
  CHECK(mesh.remove_tile(TileCoord{1, 1}));
  CHECK(mesh.tile_count() == 3);
  REQUIRE(mesh.find_path(Vec3(8.0f, 0.5f, 8.0f), Vec3(120.0f, 0.5f, 120.0f), walk_filter(),
                         std::span<Vec3>(corridor), result) == Status::NotFound);
}
