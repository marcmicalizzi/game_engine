// The ground from the world's tiles, on the CPU (docs/subsystems/renderer.md, "The ground from the
// world's tiles"; ADR-0050): the ring's first-fill rule; a tile's mesh — watertight inside, its
// border locked and on the world's lattice, and along a coarser tile collapsed onto that tile's
// lattice so the two tiles have the same edge and no T-junction, with every shared vertex naming
// the level that draws it; and the set — a tile's key from its neighbours, a rebuild that keeps
// what did not change and rebuilds what did, windows that cover every tile they draw, and a tile no
// window covers withheld. No device and no generator: heights come from a tile source of the
// test's own. The GPU half — the seams by the visibility buffer, the grid's picture to the pixel,
// the time-lapse across tiles — is terrain_tiles_gpu_tests.cpp, where the terrain capability is.
#include <core/containers/hash_map.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/terrain_tiles.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>  // engine-lint: allow-std-container a test's edge census
#include <set>  // engine-lint: allow-std-container a test's edge census
#include <string>
#include <tuple>

using namespace engine;
using namespace engine::renderer;

namespace {

// The camera on the ground plane at float32 metres by the origin, exactly widened: what these tests
// hand the tile set and the first-fill rule (`systems/world`'s ring does the same in f64).
WorldPos ground_at(f32 x, f32 z) noexcept {
  return WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)};
}

// A ground of the test's own as a tile source: gentle waves, a function of the lattice point.
f32 wave_at(f64 x, f64 z) noexcept {
  return static_cast<f32>(3.0 * std::sin(0.11 * x) * std::cos(0.07 * z) + 0.02 * x);
}
bool wave_heights(const void*, f64, i64 spacing_mm, i64, i32 i0, i32 j0, u32 nx, u32 nz, u32 begin,
                  u32 end, std::span<f32> out) noexcept {
  if (out.size() != static_cast<usize>(nx) * nz) return false;
  const u32 per_side = (nx + scene_gen::k_height_block - 1) / scene_gen::k_height_block;
  for (u32 b = begin; b < end; ++b) {
    const u32 bx = (b % per_side) * scene_gen::k_height_block;
    const u32 bz = (b / per_side) * scene_gen::k_height_block;
    for (u32 z = bz; z < std::min(nz, bz + scene_gen::k_height_block); ++z) {
      for (u32 x = bx; x < std::min(nx, bx + scene_gen::k_height_block); ++x) {
        const f64 wx = static_cast<f64>((i0 + static_cast<i64>(x)) * spacing_mm) / 1000.0;
        const f64 wz = static_cast<f64>((j0 + static_cast<i64>(z)) * spacing_mm) / 1000.0;
        out[static_cast<usize>(z) * nx + x] = wave_at(wx, wz);
      }
    }
  }
  return true;
}
constexpr scene_gen::TileSourceOps k_wave_ops{.heights = &wave_heights};
const scene_gen::TileSource k_waves{&k_wave_ops, nullptr};

TerrainDesc grid_desc() {
  TerrainDesc desc;
  desc.enabled = true;
  desc.size = 3;
  desc.extent = 64.0f;
  return desc;
}

// A small world: 8 m tiles, rings of 1.5, 3 and 6 tiles at 16, 8 and 4 cells a side.
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

// A tile's mesh built as the set builds it, from the waves, with the neighbours given.
TerrainTileMesh mesh_of(i32 x, i32 z, u8 level, u32 cells, TerrainTileNeighbours n,
                        const u32 (&level_cells)[4], i64 tile_mm = 8000) {
  TerrainTileMeshSpec spec;
  spec.x = x;
  spec.z = z;
  spec.level = level;
  spec.cells = cells;
  spec.spacing_mm = tile_mm / cells;
  spec.neighbours = n;
  for (u32 l = 0; l < 4; ++l)
    spec.level_cells[l] = level_cells[l];
  spec.uv_x0_mm = -64000;
  spec.uv_z0_mm = -64000;
  spec.uv_size_mm = 128000;
  const u32 a = cells + 3;
  Vector<f32> h(static_cast<usize>(a) * a);
  REQUIRE(k_waves.heights(0.0, spec.spacing_mm, x * static_cast<i32>(cells) - 1,
                          z * static_cast<i32>(cells) - 1, a, a,
                          std::span<f32>(h.data(), h.size())));
  TerrainTileMesh mesh;
  build_terrain_tile_mesh(spec, std::span<const f32>(h.data(), h.size()), mesh);
  return mesh;
}

using Point = std::pair<i64, i64>;  // millimetres
using Segment = std::pair<Point, Point>;

Point point_of(const TerrainTileMesh& m, u32 v, i64 spacing_mm) {
  return {static_cast<i64>(m.lattice_i[v]) * spacing_mm,
          static_cast<i64>(m.lattice_j[v]) * spacing_mm};
}

// The mesh's boundary: every edge one triangle uses, as millimetre points, in a canonical order.
std::set<Segment> boundary_of(const TerrainTileMesh& m, i64 spacing_mm) {
  std::map<Segment, i32> count;
  for (u32 t = 0; t < m.indices.size(); t += 3) {
    for (u32 e = 0; e < 3; ++e) {
      Point a = point_of(m, m.indices[t + e], spacing_mm);
      Point b = point_of(m, m.indices[t + (e + 1) % 3], spacing_mm);
      if (b < a) std::swap(a, b);
      ++count[{a, b}];
    }
  }
  std::set<Segment> out;
  for (const auto& [seg, n] : count) {
    CHECK(n <= 2);  // a manifold: no edge in three triangles
    if (n == 1) out.insert(seg);
  }
  return out;
}

// Twice the signed area of the mesh's triangles on the ground, square millimetres: the tile's own
// area when the triangles cover it once and face up.
i64 twice_area(const TerrainTileMesh& m, i64 spacing_mm, i64& flipped) {
  i64 sum = 0;
  flipped = 0;
  for (u32 t = 0; t < m.indices.size(); t += 3) {
    const Point a = point_of(m, m.indices[t], spacing_mm);
    const Point b = point_of(m, m.indices[t + 1], spacing_mm);
    const Point c = point_of(m, m.indices[t + 2], spacing_mm);
    // Counter-clockwise seen from +y in x-z is clockwise in (x, z): negate the (x, z) cross.
    const i64 cross = -((b.first - a.first) * (c.second - a.second) -
                        (b.second - a.second) * (c.first - a.first));
    if (cross <= 0) ++flipped;
    sum += cross;
  }
  return sum;
}

}  // namespace

TEST_CASE("world tiles: the cells rule, and what a description may not be") {
  TerrainTilesDesc t;
  CHECK(terrain_tile_cells(t, 0) == 64);
  CHECK(terrain_tile_cells(t, 1) == 32);
  CHECK(terrain_tile_cells(t, 2) == 16);
  CHECK(terrain_tile_cells(t, 3) == 8);
  CHECK(terrain_tile_cells(t, 6) == 8);
  t.cells[1] = 40;
  CHECK(terrain_tile_cells(t, 1) == 40);
  std::string why;
  CHECK_FALSE(validate_terrain_tiles(
      t, &why));  // 40 cuts 32 m into whole millimetres, and does not divide 64
  CHECK(why.find("must divide the 64") != std::string::npos);
  t = TerrainTilesDesc{};
  CHECK(validate_terrain_tiles(t, &why));
  t.tile_size = 32.0005f;
  CHECK_FALSE(validate_terrain_tiles(t, &why));
  CHECK(why.find("whole number of millimetres") != std::string::npos);
  t = TerrainTilesDesc{};
  t.radius[1] = 1.0f;
  CHECK_FALSE(validate_terrain_tiles(t, &why));
  t = TerrainTilesDesc{};
  t.cells[0] = 7;  // 32,000 mm over 7 is not whole
  CHECK_FALSE(validate_terrain_tiles(t, &why));
  t = TerrainTilesDesc{};
  t.cells[2] = 64;  // finer than the ring inside it
  CHECK_FALSE(validate_terrain_tiles(t, &why));
  t = TerrainTilesDesc{};
  t.ring_count = 0;
  CHECK_FALSE(validate_terrain_tiles(t, &why));
}

TEST_CASE("world tiles: the first fill is every tile within the outer radius, banded by distance") {
  const TerrainTilesDesc t = small_tiles();
  for (const Vec2 camera :
       {Vec2{0.0f, 0.0f}, Vec2{3.0f, -5.5f}, Vec2{-200.25f, 71.0f}, Vec2{50'000.0f, -3.0f}}) {
    Vector<TerrainTile> tiles;
    terrain_tiles_round(t, ground_at(camera.x, camera.y), tiles);
    // Brute force over a wide square: a tile's centre strictly inside a ring's radius is in the
    // first such ring, and anything past the outer one is not held.
    u32 expected = 0;
    const i32 cx = static_cast<i32>(std::floor(camera.x / t.tile_size));
    const i32 cz = static_cast<i32>(std::floor(camera.y / t.tile_size));
    for (i32 i = cx - 10; i <= cx + 10; ++i) {
      for (i32 j = cz - 10; j <= cz + 10; ++j) {
        const Vec3 c{(static_cast<f32>(i) + 0.5f) * t.tile_size, 0.0f,
                     (static_cast<f32>(j) + 0.5f) * t.tile_size};
        const f32 d = distance(c, Vec3{camera.x, 0.0f, camera.y});
        i32 ring = -1;
        for (u32 r = 0; r < t.ring_count && ring < 0; ++r) {
          if (d < t.radius[r] * t.tile_size) ring = static_cast<i32>(r);
        }
        if (ring < 0) continue;
        ++expected;
        const auto it = std::find_if(tiles.begin(), tiles.end(),
                                     [&](const TerrainTile& x) { return x.x == i && x.z == j; });
        REQUIRE(it != tiles.end());
        CHECK(it->ring == ring);
      }
    }
    CHECK(tiles.size() == expected);
    // In the world's tile order, x then z.
    for (u32 k = 1; k < tiles.size(); ++k) {
      CHECK((tiles[k - 1].x < tiles[k].x ||
             (tiles[k - 1].x == tiles[k].x && tiles[k - 1].z < tiles[k].z)));
    }
  }
}

TEST_CASE("world tiles: a tile's mesh covers it once, face up, its border locked on the lattice") {
  const u32 level_cells[4] = {0, 4, 8, 16};
  const TerrainTileMesh m = mesh_of(2, -3, 3, 16, TerrainTileNeighbours{}, level_cells);
  CHECK(m.indices.size() == 16u * 16u * 6u);
  CHECK(m.positions.size() == 17u * 17u);
  // The corner is the tile's lattice point (0, 0), in whole millimetres: tile (2, -3) of 8 m.
  CHECK(m.corner_x_mm == 16'000);
  CHECK(m.corner_z_mm == -24'000);
  i64 flipped = 0;
  CHECK(twice_area(m, 500, flipped) == 2 * 8000 * 8000);
  CHECK(flipped == 0);
  u32 bad = 0;
  for (u32 v = 0; v < m.positions.size(); ++v) {
    const i32 li = m.lattice_i[v] - 2 * 16;
    const i32 lj = m.lattice_j[v] + 3 * 16;
    const bool border = li == 0 || lj == 0 || li == 16 || lj == 16;
    bad += (m.locked[v] != 0) != border;
    // Positions on the world's lattice, exactly, from the tile's corner (renderer.md, "The ground's
    // tiles are placed at their corners"), and the heights the source gave there.
    bad += m.positions[v].x != static_cast<f32>(m.lattice_i[v] * 0.5 - 16.0);
    bad += m.positions[v].z != static_cast<f32>(m.lattice_j[v] * 0.5 + 24.0);
    bad += m.positions[v].y != wave_at(m.lattice_i[v] * 0.5, m.lattice_j[v] * 0.5);
    // No neighbour is coarser: every vertex is drawn from its own level, with a real normal.
    bad += m.drawn_from[v] != 3;
    bad += !(m.normals[v].y > 0.5f);
  }
  CHECK(bad == 0);
}

TEST_CASE("world tiles: a tile beside a coarser one has its edge and draws its vertices from it") {
  // Tile (0, 0) at level 3 (16 cells) with the tile to its +x at level 2 (8 cells), the one to its
  // -z at level 1 (4 cells), and the corner (+x, -z) at level 1 too.
  const u32 level_cells[4] = {0, 4, 8, 16};
  TerrainTileNeighbours n;
  n.edge[0] = 3;    // -x: the same level
  n.edge[1] = 2;    // +x: coarser by two
  n.edge[2] = 1;    // -z: coarser by four
  n.edge[3] = 3;    // +z: the same
  n.corner[0] = 1;  // (-x, -z): the coarsest round it is the -z tile's level
  n.corner[1] = 1;  // (+x, -z)
  n.corner[2] = 3;
  n.corner[3] = 2;  // (+x, +z): the +x tile's
  const TerrainTileMesh fine = mesh_of(0, 0, 3, 16, n, level_cells);
  i64 flipped = 0;
  CHECK(twice_area(fine, 500, flipped) == 2 * 8000 * 8000);
  CHECK(flipped == 0);
  // The coarser tiles' own meshes, with no coarser neighbour of their own.
  TerrainTileNeighbours none;
  const TerrainTileMesh east = mesh_of(1, 0, 2, 8, none, level_cells);
  const TerrainTileMesh south = mesh_of(0, -1, 1, 4, none, level_cells);
  const std::set<Segment> fb = boundary_of(fine, 500);
  const std::set<Segment> eb = boundary_of(east, 1000);
  const std::set<Segment> sb = boundary_of(south, 2000);
  // Along x = 8 m the fine tile's boundary is exactly the east tile's: the same segments, so no
  // vertex of one lies inside an edge of the other.
  const auto on = [](const std::set<Segment>& b, auto&& pred) {
    std::set<Segment> out;
    for (const Segment& s : b)
      if (pred(s)) out.insert(s);
    return out;
  };
  const auto at_x = [](i64 x) {
    return [x](const Segment& s) { return s.first.first == x && s.second.first == x; };
  };
  const auto at_z = [](i64 z) {
    return [z](const Segment& s) { return s.first.second == z && s.second.second == z; };
  };
  const std::set<Segment> fine_east = on(fb, at_x(8000));
  const std::set<Segment> east_west = on(eb, at_x(8000));
  CHECK(fine_east.size() == 8);
  CHECK(fine_east == east_west);
  const std::set<Segment> fine_south = on(fb, at_z(0));
  const std::set<Segment> south_north = on(sb, at_z(0));
  CHECK(fine_south.size() == 4);
  CHECK(fine_south == south_north);
  // The edges along tiles of its own level keep every vertex.
  CHECK(on(fb, at_x(0)).size() == 16);
  CHECK(on(fb, at_z(8000)).size() == 16);
  // And every vertex it shares with a coarser tile names that tile's level, in a horizontal rest
  // normal that decodes to it; the corners name the coarsest tile round them.
  u32 named = 0;
  u32 bad = 0;
  for (u32 v = 0; v < fine.positions.size(); ++v) {
    const i32 i = fine.lattice_i[v];
    const i32 j = fine.lattice_j[v];
    u8 want = 3;
    if ((i == 0 || i == 16) && (j == 0 || j == 16)) {
      want = i == 0 && j == 0 ? 1 : i == 16 && j == 0 ? 1 : i == 16 && j == 16 ? 2 : 3;
    } else if (j == 0) {
      want = 1;
    } else if (i == 16) {
      want = 2;
    }
    bad += fine.drawn_from[v] != want;
    if (want != 3) {
      ++named;
      const Vec3 nrm = fine.normals[v];
      const i32 sixteenths = static_cast<i32>(
          std::lround(static_cast<f64>(std::atan2(nrm.z, nrm.x)) * 8.0 / 3.14159265358979));
      bad += !(std::abs(nrm.y) < 0.25f);
      bad += static_cast<u32>((sixteenths + 16) % 16) != want;
      // A vertex drawn from a coarser level is on that level's lattice.
      const i64 coarse = want == 1 ? 2000 : 1000;
      bad += (static_cast<i64>(i) * 500) % coarse != 0 || (static_cast<i64>(j) * 500) % coarse != 0;
    }
  }
  CHECK(bad == 0);
  // The -z edge's 5 kept vertices (a 4-cell tile's lattice), and the +x edge's 9 (an 8-cell
  // tile's) less the corner they share.
  CHECK(named == 5 + 9 - 1);
}

TEST_CASE("world tiles: a rebuild of what changed draws what a whole build draws") {
  // renderer.md, "What a frame waits for": a rebuild touches the tiles that entered, left or
  // changed level and their neighbours, and nothing else — and must land on exactly the chunks a
  // set built whole round the same camera has, tile for tile, level for level and key for key.
  // A camera flies a curve across 30 tiles in steps of a third of a tile, handing the set the
  // first-fill rule's tiles as changes; at every step the set is compared with one built afresh.
  const TerrainDesc grid = grid_desc();
  const TerrainTilesDesc t = small_tiles();
  TerrainTileSet set;
  std::string error;
  REQUIRE_MESSAGE(set.build(grid, t, k_waves, ground_at(1.0f, 2.0f), nullptr, &error), error);
  Vector<TerrainTile> held;
  terrain_tiles_round(t, ground_at(1.0f, 2.0f), held);
  u32 most_built = 0;
  u32 rebuilds = 0;
  for (u32 step = 1; step <= 90; ++step) {
    const f32 x = 1.0f + static_cast<f32>(step) * t.tile_size / 3.0f;
    const f32 z = 2.0f + 40.0f * std::sin(static_cast<f32>(step) * 0.05f);
    Vector<TerrainTile> next;
    terrain_tiles_round(t, ground_at(x, z), next);
    // The world's events: what entered or changed ring, and what left.
    Vector<TerrainTile> changes;
    for (const TerrainTile& n : next) {
      bool same = false;
      for (const TerrainTile& h : held)
        same = same || (h.x == n.x && h.z == n.z && h.ring == n.ring);
      if (!same) changes.push_back(n);
    }
    for (const TerrainTile& h : held) {
      bool still = false;
      for (const TerrainTile& n : next)
        still = still || (h.x == n.x && h.z == n.z);
      if (!still) changes.push_back(TerrainTile{h.x, h.z, k_tile_gone});
    }
    held = next;
    const bool changed =
        set.change_tiles(std::span<const TerrainTile>(changes.data(), changes.size()));
    CHECK(changed == !changes.empty());
    const TerrainRingLayout target = set.next_layout(ground_at(x, z), set.layout());
    if (target == set.layout()) continue;
    set.prepare(target);
    u32 moved = 0;
    REQUIRE_MESSAGE(set.update(ground_at(x, z), 0.0, target, {}, nullptr, moved, &error), error);
    CHECK(set.changed_only());
    ++rebuilds;
    most_built = std::max(most_built, set.last_built());
    // What it changed is what it says it changed: every added index names a chunk with no slot.
    for (u32 l = 1; l < set.level_count(); ++l) {
      for (const u32 c : set.added(l)) {
        REQUIRE(c < set.chunks(l).size());
        CHECK(set.chunks(l)[c].slot == ~0u);
      }
    }
    TerrainTileSet whole;
    REQUIRE_MESSAGE(whole.build(grid, t, k_waves, ground_at(x, z), nullptr, &error), error);
    u32 drawn = 0;
    u32 differ = 0;
    for (u32 l = 1; l < set.level_count(); ++l) {
      drawn += set.chunks(l).size();
      for (const TerrainChunk& c : whole.chunks(l)) {
        u32 level = 0;
        u32 index = 0;
        if (!set.find(c.i, c.j, level, index) || level != l || set.chunks(l)[index].key != c.key)
          ++differ;
      }
    }
    u32 whole_drawn = 0;
    for (u32 l = 1; l < whole.level_count(); ++l)
      whole_drawn += whole.chunks(l).size();
    CHECK(differ == 0);
    CHECK(drawn == whole_drawn);
    CHECK(set.withheld() == 0);
  }
  MESSAGE(rebuilds << " rebuilds of changes, at most " << most_built << " tiles built in one");
  CHECK(rebuilds > 30);
  // A third of a tile's step changes a band of tiles, never the whole set.
  CHECK(most_built < held.size() / 2);
}

TEST_CASE("world tiles: a set's keys, rebuilds, windows and what it withholds") {
  const TerrainDesc grid = grid_desc();
  const TerrainTilesDesc t = small_tiles();
  TerrainTileSet set;
  std::string error;
  REQUIRE_MESSAGE(set.build(grid, t, k_waves, ground_at(1.0f, 2.0f), nullptr, &error), error);
  REQUIRE(set.valid());
  CHECK(set.level_count() == 4);
  CHECK_FALSE(set.grid_drawn());
  CHECK(set.shares_vertices());
  CHECK(set.cells(1) == 4);
  CHECK(set.cells(3) == 16);
  CHECK(set.lattice(3).spacing_mm == 500);
  // The first layout is the first-fill rule's, every tile drawn at its ring's level.
  Vector<TerrainTile> first;
  terrain_tiles_round(t, ground_at(1.0f, 2.0f), first);
  u32 drawn = 0;
  for (u32 l = 1; l < 4; ++l)
    drawn += set.chunks(l).size();
  CHECK(drawn == first.size());
  CHECK(set.withheld() == 0);
  for (const TerrainTile& tile : first) {
    u32 level = 0;
    u32 index = 0;
    REQUIRE(set.find(tile.x, tile.z, level, index));
    CHECK(level == set.level_of_ring(tile.ring));
    const TerrainChunk& chunk = set.chunks(level)[index];
    CHECK_FALSE(chunk.lod.mesh.clusters.empty());
    CHECK(chunk.slot == ~0u);
    // Its level's window covers it and a point of apron.
    const gfx::TerrainField w = set.field_window(level, set.layout());
    const i32 c = static_cast<i32>(set.cells(level));
    CHECK(w.i0 <= tile.x * c - 1);
    CHECK(w.j0 <= tile.z * c - 1);
    CHECK(w.i0 + static_cast<i32>(w.nx) >= (tile.x + 1) * c + 2);
    CHECK(w.j0 + static_cast<i32>(w.nz) >= (tile.z + 1) * c + 2);
    CHECK(u64{w.nx} * w.nz <= set.field_capacity(level));
  }
  // Every level has room for twice what its ring can hold.
  for (u32 l = 1; l < 4; ++l) {
    CHECK(set.capacity(l).slots >= 2 * set.chunks(l).size());
    CHECK(set.capacity(l).clusters_per_slot > 0);
  }
  // The same set handed over again changes nothing and asks for nothing.
  const TerrainRingLayout shown = set.layout();
  CHECK_FALSE(set.set_tiles(std::span<const TerrainTile>(first.data(), first.size())));
  CHECK(set.next_layout(ground_at(1.0f, 2.0f), shown) == shown);

  // One tile moves out a ring (its level changes) — it and every tile whose mesh depends on it are
  // rebuilt, and nothing else.
  Vector<TerrainTile> next = first;
  u32 moved_tile = ~0u;
  for (u32 k = 0; k < next.size(); ++k) {
    if (next[k].ring == 1 && next[k].x == 1 && next[k].z == -1) moved_tile = k;
  }
  REQUIRE(moved_tile != ~0u);
  next[moved_tile].ring = 2;
  CHECK(set.set_tiles(std::span<const TerrainTile>(next.data(), next.size())));
  const TerrainRingLayout target = set.next_layout(ground_at(1.0f, 2.0f), shown);
  CHECK(target.generation == shown.generation + 1);
  set.prepare(target);
  u32 moved = 0;
  REQUIRE(set.update(ground_at(1.0f, 2.0f), 0.0, target, {}, nullptr, moved, &error));
  CHECK(set.layout() == target);
  CHECK((moved & (1u << set.level_of_ring(1))) != 0);
  CHECK((moved & (1u << set.level_of_ring(2))) != 0);
  // The tile itself, and at most its eight neighbours, were built again.
  CHECK(set.last_built() >= 1);
  CHECK(set.last_built() <= 9);
  CHECK(set.last_kept() + set.last_built() == first.size());

  // A tile far outside every window (a deactivation a world's budget deferred) is withheld, and
  // the window moves to the camera when it has gone past its margin.
  next.push_back(TerrainTile{400, 400, 2});
  CHECK(set.set_tiles(std::span<const TerrainTile>(next.data(), next.size())));
  const TerrainRingLayout far = set.next_layout(ground_at(1.0f, 2.0f), set.layout());
  set.prepare(far);
  REQUIRE(set.update(ground_at(1.0f, 2.0f), 0.0, far, {}, nullptr, moved, &error));
  CHECK(set.withheld() >= 1);
  u32 level = 0;
  u32 index = 0;
  CHECK_FALSE(set.find(400, 400, level, index));
  const TerrainRingLayout moved_on =
      set.next_layout(ground_at(1.0f + 5.0f * 8.0f, 2.0f), set.layout());
  CHECK(moved_on.cx[3] != set.layout().cx[3]);
}

// ---- the far levels (renderer.md, "Ground to the horizon") -------------------------------------

namespace {

// The small world with three far levels past it: the first at twice the outermost ring's 2 m, four
// cells a far tile — 16 m tiles of two world tiles a side, 32 m and 64 m after it.
TerrainTilesDesc small_far_tiles() {
  TerrainTilesDesc t = small_tiles();
  t.far_levels = 3;
  t.far_cells = 4;
  t.far_ratio = 2;
  return t;
}

// What every drawn chunk's mesh makes together, rings' and far levels' alike, each built from the
// spec the set derives for it: the keys that differ from the ones its chunks were built with,
// twice the area its triangles cover and those that face down, and its boundary — every edge a
// mesh's triangles use once — counted over all the meshes. An edge inside the ground is two meshes'
// (a seam with no T-junction: both sides have the same vertices along it), the outermost square's
// border one mesh's.
struct Tiling {
  u32 chunks = 0;
  u32 bad_keys = 0;
  i64 area2 = 0;
  i64 flipped = 0;
  u32 inner_edges = 0;
  u32 unmatched = 0;  // an inner edge not exactly two meshes'
  u32 border_bad = 0;
  i64 lo_x = 0, hi_x = 0, lo_z = 0, hi_z = 0;
};

Tiling tiling(const TerrainTileSet& set) {
  Tiling out;
  std::map<Segment, i32> count;
  for (u32 l = 1; l < set.level_count(); ++l) {
    for (u32 c = 0; c < set.chunks(l).size(); ++c) {
      TerrainTileMeshSpec spec;
      REQUIRE(set.chunk_spec(l, c, spec));
      ++out.chunks;
      out.bad_keys += terrain_tile_key(spec) != set.chunks(l)[c].key;
      const u32 a = spec.cells + 3;
      Vector<f32> h(static_cast<usize>(a) * a);
      REQUIRE(k_waves.filtered(
          0.0, spec.spacing_mm, set.lattice(l).filter_mm, spec.x * static_cast<i32>(spec.cells) - 1,
          spec.z * static_cast<i32>(spec.cells) - 1, a, a, std::span<f32>(h.data(), h.size())));
      TerrainTileMesh mesh;
      build_terrain_tile_mesh(spec, std::span<const f32>(h.data(), h.size()), mesh);
      i64 flipped = 0;
      out.area2 += twice_area(mesh, spec.spacing_mm, flipped);
      out.flipped += flipped;
      if (flipped != 0 && out.flipped == flipped) {
        MESSAGE("flipped: level " << l << " tile " << spec.x << "," << spec.z << " cells "
                                  << spec.cells << " hole " << spec.hole_mask << " edges "
                                  << int{spec.neighbours.edge[0]} << int{spec.neighbours.edge[1]}
                                  << int{spec.neighbours.edge[2]} << int{spec.neighbours.edge[3]}
                                  << " corners " << int{spec.neighbours.corner[0]}
                                  << int{spec.neighbours.corner[1]}
                                  << int{spec.neighbours.corner[2]}
                                  << int{spec.neighbours.corner[3]} << " count " << flipped);
      }
      for (const Segment& s : boundary_of(mesh, spec.spacing_mm))
        ++count[s];
    }
  }
  const TerrainRingLayout layout = set.layout();
  const u32 coarsest = set.far_count() > 0 ? 1u : set.level_count() - 1;
  out.lo_x = layout.cx[coarsest] - layout.half[coarsest];
  out.hi_x = layout.cx[coarsest] + layout.half[coarsest];
  out.lo_z = layout.cz[coarsest] - layout.half[coarsest];
  out.hi_z = layout.cz[coarsest] + layout.half[coarsest];
  for (const auto& [seg, n] : count) {
    const auto on_x = [&](i64 v) { return seg.first.first == v && seg.second.first == v; };
    const auto on_z = [&](i64 v) { return seg.first.second == v && seg.second.second == v; };
    const bool border = on_x(out.lo_x) || on_x(out.hi_x) || on_z(out.lo_z) || on_z(out.hi_z);
    if (border) {
      out.border_bad += n != 1;
    } else {
      ++out.inner_edges;
      out.unmatched += n != 2;
    }
  }
  return out;
}

void check_tiling(const TerrainTileSet& set, const char* where) {
  const Tiling t = tiling(set);
  CAPTURE(where);
  CHECK(t.chunks > 0);
  CHECK(t.bad_keys == 0);
  CHECK(t.flipped == 0);
  CHECK(t.unmatched == 0);
  CHECK(t.border_bad == 0);
  // The coarsest far level's square, covered once: every tile the rings draw and every far tile.
  const i64 side = t.hi_x - t.lo_x;
  CHECK(t.area2 == 2 * side * side);
}

}  // namespace

TEST_CASE("world tiles: the far levels tile the ground to their outermost square, seamed") {
  const TerrainDesc grid = grid_desc();
  const TerrainTilesDesc t = small_far_tiles();
  std::string error;
  REQUIRE_MESSAGE(validate_terrain_tiles(t, &error), error);
  // The far levels' arithmetic: twice the spacing, the tile and the reach each, the first square
  // holding every tile the rings can hold from wherever its centre may be.
  const TerrainFarLevel f1 = terrain_far_level(t, 1);
  const TerrainFarLevel f2 = terrain_far_level(t, 2);
  const TerrainFarLevel f3 = terrain_far_level(t, 3);
  CHECK(f1.spacing == 4000);
  CHECK(f1.tile == 16000);
  CHECK(f1.snap == 32000);
  CHECK(f2.spacing == 8000);
  CHECK(f3.tile == 64000);
  CHECK(f2.half == 2 * f1.half);
  CHECK(f3.half == 4 * f1.half);
  CHECK(f1.half % f1.snap == 0);
  CHECK(f1.half >= 6 * f1.snap);
  const f64 rings_reach = (6.0 * 1.15 + 1.75) * 8.0;
  CHECK(static_cast<f64>(f1.half - f1.margin - f1.snap / 2) / 1000.0 >= rings_reach);
  CHECK(terrain_far_reach_m(t) ==
        doctest::Approx(static_cast<f64>(f3.half - f3.margin - f3.snap / 2) / 1000.0));
  {
    TerrainTilesDesc bad = t;
    bad.far_ratio = 1;
    CHECK_FALSE(validate_terrain_tiles(bad, &error));
    bad = t;
    bad.far_cells = 3;  // a 12 m far tile is not whole 8 m world tiles
    CHECK_FALSE(validate_terrain_tiles(bad, &error));
    bad = t;
    bad.far_levels = k_max_far_levels + 1;
    CHECK_FALSE(validate_terrain_tiles(bad, &error));
  }

  TerrainTileSet set;
  REQUIRE_MESSAGE(set.build(grid, t, k_waves, ground_at(1.0f, 2.0f), nullptr, &error), error);
  CHECK(set.level_count() == 7);
  CHECK(set.far_count() == 3);
  CHECK(set.is_far(1));
  CHECK(set.is_far(3));
  CHECK_FALSE(set.is_far(4));
  CHECK(set.level_of_ring(2) == 4);  // the outermost ring, past the far levels
  CHECK(set.level_of_ring(0) == 6);
  CHECK(set.lattice(3).spacing_mm == 4000);
  CHECK(set.lattice(3).filter_mm == 4000);  // a far level's heights are filtered to its spacing
  CHECK(set.lattice(4).filter_mm == 0);     // a ring's are the ground's own
  CHECK(set.withheld() == 0);
  for (u32 l = 1; l <= 3; ++l) {
    CHECK(set.capacity(l).slots >= 2 * set.chunks(l).size());
    CHECK(set.field_capacity(l) >=
          u64{set.field_window(l, set.layout()).nx} * set.field_window(l, set.layout()).nz);
  }
  check_tiling(set, "the first layout");
  // A ring's tile beside the first far level collapses its edge onto that level's lattice and
  // names it: the ring tiles keep every vertex they had along each other.
  const Tiling first = tiling(set);
  MESSAGE(first.chunks << " chunks, " << first.inner_edges << " inner edges");

  // **Flying 300 m east and 120 m north**, the rings handed over by the first-fill rule a step at
  // a time and every far square following by its margin: after every rebuild the ground is
  // still covered once and seamed, and the far levels' windows moved.
  Vector<TerrainTile> round;
  f32 x = 1.0f;
  f32 z = 2.0f;
  u32 far_moves = 0;
  for (u32 k = 0; k < 110; ++k) {
    x += 2.7f;
    z += 1.1f;
    terrain_tiles_round(t, ground_at(x, z), round);
    set.set_tiles(std::span<const TerrainTile>(round.data(), round.size()));
    const TerrainRingLayout target = set.next_layout(ground_at(x, z), set.layout());
    for (u32 l = 1; l <= 3; ++l)
      far_moves += target.cx[l] != set.layout().cx[l] || target.cz[l] != set.layout().cz[l];
    set.prepare(target);
    u32 moved = 0;
    REQUIRE_MESSAGE(set.update(ground_at(x, z), 0.0, target, {}, nullptr, moved, &error), error);
    if (k % 11 == 10) check_tiling(set, "in flight");
  }
  CHECK(far_moves >= 4);
  CHECK(set.withheld() == 0);
  check_tiling(set, "after the flight");

  // **Fifty kilometres out**: the same arithmetic, the same seams.
  TerrainTileSet out;
  REQUIRE_MESSAGE(out.build(grid, t, k_waves, ground_at(50'000.0f, -50'000.0f), nullptr, &error),
                  error);
  check_tiling(out, "50 km out");
}
