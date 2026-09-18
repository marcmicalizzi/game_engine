// Tile building: what the parameters refuse, what a tile contains, and that the same input
// always produces the same bytes.

#include "nav_test_support.h"

#include <core/containers/vector.h>
#include <domain/nav/tile.h>

#include <doctest/doctest.h>

#include <cstring>

using namespace engine;
using namespace engine::nav;
using namespace engine::nav::testing;

TEST_CASE("nav: a tile builds from a floor and covers it") {
  const NavBuildParams params = test_params();
  const Soup soup = floor_with_wall(0.0f);
  NavTileData tile;
  TileBuildStats stats;
  REQUIRE(build_tile(params, TileCoord{0, 0}, soup.view(), tile, &stats) == Status::Ok);

  CHECK(tile.walkable());
  CHECK(tile.poly_count() > 0);
  CHECK(tile.region_count() == 1);
  CHECK(stats.triangles_in == soup.indices.size() / 3);
  CHECK(stats.triangles_rasterized == stats.triangles_in);

  // The tile's polygon mesh spans exactly the grid cell it was asked for, because Recast
  // re-bases the contour set to the inner tile after building with a border.
  const Aabb3 bounds = tile.bounds();
  CHECK(bounds.min.x == doctest::Approx(0.0f).epsilon(0.01));
  CHECK(bounds.min.z == doctest::Approx(0.0f).epsilon(0.01));
  CHECK(bounds.max.x == doctest::Approx(64.0f).epsilon(0.01));
  CHECK(bounds.max.z == doctest::Approx(64.0f).epsilon(0.01));

  CHECK(tile.grid_tile_size() == doctest::Approx(64.0f));
  CHECK(tile.agent_radius() == doctest::Approx(params.agent_radius));
  CHECK(tile.coord() == TileCoord{0, 0});

  // One region, and it covers most of the floor: 64 x 64 m less the radius eroded off the edge.
  REQUIRE(tile.regions().size() == 1);
  CHECK(tile.regions()[0].area > 3000.0f);
  CHECK(tile.regions()[0].area < 64.0f * 64.0f);
}

TEST_CASE("nav: a wall splits a tile into two regions and shows up as portals") {
  const NavBuildParams params = test_params();
  // The floor runs eight metres past the tile on every side, which is what a caller has to hand
  // over for a tile to have border portals at all: the agent radius is eroded off the walkable
  // area, so geometry that stops at the tile border leaves nothing touching it.
  const Soup soup = extended_floor_with_wall(8.0f);
  NavTileData tile;
  REQUIRE(build_tile(params, TileCoord{0, 0}, soup.view(), tile, nullptr) == Status::Ok);
  CHECK(tile.region_count() == 2);

  bool region_0_has_portal = false;
  bool region_1_has_portal = false;
  for (const TilePortal& portal : tile.portals()) {
    CHECK(portal.side < 4);
    if (portal.region == 0) region_0_has_portal = true;
    if (portal.region == 1) region_1_has_portal = true;
  }
  CHECK(region_0_has_portal);
  CHECK(region_1_has_portal);

  // The same floor with no margin has the two regions and no portals at all.
  const Soup flush = floor_with_wall(64.0f);
  NavTileData flush_tile;
  REQUIRE(build_tile(params, TileCoord{0, 0}, flush.view(), flush_tile, nullptr) == Status::Ok);
  CHECK(flush_tile.region_count() == 2);
  CHECK(flush_tile.portals().empty());
}

TEST_CASE("nav: the same input builds the same bytes") {
  const NavBuildParams params = test_params();
  const Soup soup = floor_with_wall(48.0f);

  NavTileData first;
  NavTileData second;
  REQUIRE(build_tile(params, TileCoord{0, 0}, soup.view(), first, nullptr) == Status::Ok);
  REQUIRE(build_tile(params, TileCoord{0, 0}, soup.view(), second, nullptr) == Status::Ok);

  REQUIRE(first.bytes().size() == second.bytes().size());
  CHECK(std::memcmp(first.bytes().data(), second.bytes().data(), first.bytes().size()) == 0);

  // And a round trip through the bytes is the same tile again, which is what makes the buffer
  // usable as a cache entry.
  NavTileData reloaded;
  REQUIRE(reloaded.from_bytes(first.bytes()));
  CHECK(reloaded.poly_count() == first.poly_count());
  CHECK(reloaded.region_count() == first.region_count());
  REQUIRE(reloaded.bytes().size() == first.bytes().size());
  CHECK(std::memcmp(reloaded.bytes().data(), first.bytes().data(), first.bytes().size()) == 0);

  const NavTileData copy = first.clone();
  REQUIRE(copy.bytes().size() == first.bytes().size());
  CHECK(std::memcmp(copy.bytes().data(), first.bytes().data(), first.bytes().size()) == 0);
}

TEST_CASE("nav: an empty tile is a success, not an error") {
  const NavBuildParams params = test_params();
  const Soup soup = floor_with_wall(0.0f);
  NavTileData tile;
  // Tile (5, 5) is nowhere near the floor.
  REQUIRE(build_tile(params, TileCoord{5, 5}, soup.view(), tile, nullptr) == Status::Ok);
  CHECK_FALSE(tile.walkable());
  CHECK(tile.empty());
}

TEST_CASE("nav: a tile size the cell size does not divide is refused") {
  NavBuildParams params = test_params();
  CHECK(validate(params));

  // 64 / 0.3 is 213.33: the voxel grid would cover 63.9 m of a 64 m tile and every tile border
  // would silently stop linking. This is the check that exists because the failure is invisible.
  params.cell_size = 0.3f;
  CHECK_FALSE(validate(params));

  params = test_params();
  params.cell_size = 0.0f;
  CHECK_FALSE(validate(params));
  params = test_params();
  params.agent_max_slope_degrees = 120.0f;
  CHECK_FALSE(validate(params));
  params = test_params();
  params.verts_per_poly = 8;  // Detour's own limit is 6
  CHECK_FALSE(validate(params));
  params = test_params();
  params.tile_size = 2.0f;  // smaller than twice the border the pipeline needs
  CHECK_FALSE(validate(params));

  NavTileData tile;
  const Soup soup = floor_with_wall(0.0f);
  params = test_params();
  params.cell_size = 0.3f;
  CHECK(build_tile(params, TileCoord{0, 0}, soup.view(), tile, nullptr) == Status::InvalidArgument);
}

TEST_CASE("nav: bad geometry is refused rather than misread") {
  const NavBuildParams params = test_params();
  Soup soup = floor_with_wall(0.0f);
  NavTileData tile;

  Soup truncated = soup;
  truncated.indices.pop_back();
  CHECK(build_tile(params, TileCoord{0, 0}, truncated.view(), tile, nullptr) ==
        Status::InvalidArgument);

  Soup out_of_range = soup;
  out_of_range.indices[0] = out_of_range.vertices.size() + 10;
  CHECK(build_tile(params, TileCoord{0, 0}, out_of_range.view(), tile, nullptr) ==
        Status::InvalidArgument);

  // One area per triangle, or none at all.
  Vector<u8> areas(soup.indices.size() / 3 - 1, static_cast<u8>(Area::Ground));
  TileGeometry geometry = soup.view();
  geometry.areas = std::span<const u8>(areas.data(), areas.size());
  CHECK(build_tile(params, TileCoord{0, 0}, geometry, tile, nullptr) == Status::InvalidArgument);
}

TEST_CASE("nav: the cheap settings build a tile too") {
  // Half the voxel resolution, monotone partitioning, and no detail mesh: the configuration a
  // game drops to when the rebuild budget is tight, and the one the bench's second row uses.
  NavBuildParams params = test_params();
  params.cell_size = 0.5f;
  params.cell_height = 0.3f;
  SUBCASE("monotone") { params.monotone_regions = true; }
  SUBCASE("no detail mesh") { params.detail_sample_dist = 0.0f; }
  SUBCASE("both") {
    params.monotone_regions = true;
    params.detail_sample_dist = 0.0f;
  }
  SUBCASE("neither") {}
  const Soup soup = extended_floor_with_wall(8.0f);
  NavTileData tile;
  REQUIRE(build_tile(params, TileCoord{0, 0}, soup.view(), tile, nullptr) == Status::Ok);
  CHECK(tile.walkable());
  CHECK(tile.region_count() == 2);
}

TEST_CASE("nav: the tile grid is floor, not truncation") {
  const Vec3 origin(0.0f, 0.0f, 0.0f);
  CHECK(tile_containing(Vec3(0.5f, 0.0f, 0.5f), origin, 64.0f) == TileCoord{0, 0});
  CHECK(tile_containing(Vec3(-0.5f, 0.0f, 0.5f), origin, 64.0f) == TileCoord{-1, 0});
  CHECK(tile_containing(Vec3(-0.5f, 0.0f, -0.5f), origin, 64.0f) == TileCoord{-1, -1});
  CHECK(tile_containing(Vec3(64.5f, 0.0f, 129.0f), origin, 64.0f) == TileCoord{1, 2});

  // The key round-trips, including for negative coordinates, because a tile set is ordered by it.
  const TileCoord coords[4] = {{0, 0}, {-1, 3}, {7, -9}, {-40000, -40000}};
  for (const TileCoord coord : coords)
    CHECK(tile_from_key(tile_key(coord)) == coord);
}

TEST_CASE("nav: areas survive the pipeline and decide the flags") {
  const NavBuildParams params = test_params();
  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 16);
  // The far half of the floor is water.
  Vector<u8> areas(soup.indices.size() / 3, static_cast<u8>(Area::Ground));
  for (u32 t = 0; t < areas.size(); ++t) {
    const Vec3 a = soup.vertices[soup.indices[t * 3]];
    if (a.x > 32.0f) areas[t] = static_cast<u8>(Area::Water);
  }
  TileGeometry geometry = soup.view();
  geometry.areas = std::span<const u8>(areas.data(), areas.size());

  NavTileData tile;
  REQUIRE(build_tile(params, TileCoord{0, 0}, geometry, tile, nullptr) == Status::Ok);
  CHECK(tile.walkable());

  // A filter that excludes swimming cannot cross to the far side, one that includes it can.
  NavMesh mesh;
  REQUIRE(mesh.init(test_mesh_options(params)) == Status::Ok);
  REQUIRE(mesh.add_tile(tile) == Status::Ok);

  PathFilter no_swim;
  no_swim.include_flags = k_flag_walk;
  NavPoint point;
  CHECK(mesh.find_nearest(Vec3(50.0f, 0.5f, 32.0f), no_swim, point) == false);

  PathFilter swim;
  swim.include_flags = static_cast<u16>(k_flag_walk | k_flag_swim);
  CHECK(mesh.find_nearest(Vec3(50.0f, 0.5f, 32.0f), swim, point));
  CHECK(point.valid());
}
