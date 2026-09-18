#pragma once

// Fixtures shared by the nav tests: a triangle-soup builder, a flat floor, boxes, and the
// parameters every test starts from.
//
// The geometry is deliberately crude — axis-aligned boxes and a floor grid — because what is
// under test is the pipeline and the queue, not Recast's triangle handling, and a scene a reader
// can picture is worth more here than a realistic one.

#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/nav/nav_mesh.h>
#include <domain/nav/rebuild_queue.h>
#include <domain/nav/region_graph.h>
#include <domain/nav/tile.h>

#include <doctest/doctest.h>

namespace engine::nav::testing {

struct Soup {
  Vector<Vec3> vertices;
  Vector<u32> indices;

  TileGeometry view() const {
    TileGeometry geometry;
    geometry.vertices = std::span<const Vec3>(vertices.data(), vertices.size());
    geometry.indices = std::span<const u32>(indices.data(), indices.size());
    return geometry;
  }

  void clear() {
    vertices.clear();
    indices.clear();
  }
};

// A quad in the xz plane at height y, wound counter-clockwise seen from above.
inline void add_quad_xz(Soup& soup, f32 x0, f32 z0, f32 x1, f32 z1, f32 y) {
  const u32 base = soup.vertices.size();
  soup.vertices.push_back(Vec3(x0, y, z0));
  soup.vertices.push_back(Vec3(x0, y, z1));
  soup.vertices.push_back(Vec3(x1, y, z1));
  soup.vertices.push_back(Vec3(x1, y, z0));
  const u32 order[6] = {0, 1, 2, 0, 2, 3};
  for (const u32 i : order)
    soup.indices.push_back(base + i);
}

// A floor made of `cells` x `cells` quads, so the soup looks like the terrain a game hands over
// rather than two enormous triangles.
inline void add_floor(Soup& soup, f32 x0, f32 z0, f32 x1, f32 z1, f32 y, u32 cells = 8) {
  const f32 dx = (x1 - x0) / static_cast<f32>(cells);
  const f32 dz = (z1 - z0) / static_cast<f32>(cells);
  for (u32 i = 0; i < cells; ++i) {
    for (u32 j = 0; j < cells; ++j) {
      const f32 ax = x0 + dx * static_cast<f32>(i);
      const f32 az = z0 + dz * static_cast<f32>(j);
      add_quad_xz(soup, ax, az, ax + dx, az + dz, y);
    }
  }
}

// A closed axis-aligned box: an obstacle an agent has to walk around.
inline void add_box(Soup& soup, Vec3 lo, Vec3 hi) {
  const u32 base = soup.vertices.size();
  soup.vertices.push_back(Vec3(lo.x, lo.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, lo.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, lo.y, hi.z));
  soup.vertices.push_back(Vec3(lo.x, lo.y, hi.z));
  soup.vertices.push_back(Vec3(lo.x, hi.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, hi.y, lo.z));
  soup.vertices.push_back(Vec3(hi.x, hi.y, hi.z));
  soup.vertices.push_back(Vec3(lo.x, hi.y, hi.z));
  const u32 faces[36] = {0, 2, 1, 0, 3, 2,   // bottom
                         4, 5, 6, 4, 6, 7,   // top
                         0, 1, 5, 0, 5, 4,   // -z
                         1, 2, 6, 1, 6, 5,   // +x
                         2, 3, 7, 2, 7, 6,   // +z
                         3, 0, 4, 3, 4, 7};  // -x
  for (const u32 i : faces)
    soup.indices.push_back(base + i);
}

// The parameters every test starts from. 0.25 m cells divide the 64 m tile exactly, which is the
// rule `validate()` enforces and the one that is easiest to break by accident.
inline NavBuildParams test_params() {
  NavBuildParams params;
  params.cell_size = 0.25f;
  params.cell_height = 0.2f;
  params.tile_size = 64.0f;
  params.origin = Vec3(0.0f, 0.0f, 0.0f);
  params.agent_radius = 0.5f;
  params.agent_height = 2.0f;
  params.agent_max_climb = 0.4f;
  params.height_min = -8.0f;
  params.height_max = 32.0f;
  params.region_min_size = 2.0f;
  params.region_merge_size = 6.0f;
  return params;
}

inline NavMeshOptions test_mesh_options(const NavBuildParams& params) {
  NavMeshOptions options;
  options.origin = params.origin;
  options.tile_size = params.tile_size;
  options.max_tiles = 64;
  options.max_polys_per_tile = 4096;
  return options;
}

// A 64 x 64 m floor filling tile (0, 0), with a wall from z = 0 to z = `wall_length` at
// x = 32 m. An agent crossing at z = 24 has to walk around the wall's far end.
inline Soup floor_with_wall(f32 wall_length) {
  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 16);
  if (wall_length > 0.0f) add_box(soup, Vec3(31.5f, 0.0f, 0.0f), Vec3(32.5f, 3.0f, wall_length));
  return soup;
}

// The same tile, but with the floor reaching `margin` metres past it on every side and the wall
// running the whole way. A tile whose *geometry* stops at the tile border has no border portals
// at all — the agent radius is eroded off the edge of the walkable area, so nothing reaches
// voxel 0 or voxel N — so this is the shape a caller has to hand over for tiles to link.
inline Soup extended_floor_with_wall(f32 margin) {
  Soup soup;
  add_floor(soup, -margin, -margin, 64.0f + margin, 64.0f + margin, 0.0f, 20);
  add_box(soup, Vec3(31.5f, 0.0f, -margin), Vec3(32.5f, 3.0f, 64.0f + margin));
  return soup;
}

inline PathFilter walk_filter() { return PathFilter{}; }

// Walking only: the default filter includes every flag but `disabled`, which is right for a
// game but wrong for a test that wants to prove a jump link was not used.
inline PathFilter walk_only_filter() {
  PathFilter filter;
  filter.include_flags = k_flag_walk;
  return filter;
}

}  // namespace engine::nav::testing
