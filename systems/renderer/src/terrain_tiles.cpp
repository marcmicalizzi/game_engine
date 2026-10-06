// The ground from the world's tiles (terrain_tiles.h; docs/subsystems/renderer.md, "The ground from
// the world's tiles"; ADR-0050).
#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain_tiles.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// Read when the tiles are built (renderer.md, "The ground from the world's tiles").
tunables::Float tile_slack{"renderer.terrain.tile_slack", 1.5, 1.0, 8.0,
                           "How much room each world tile's GPU slot and its level's arenas keep "
                           "above the largest tile the first layout built, as a factor"};
// Read when a host makes its tile description (renderer.md, "Ground to the horizon"). Six: on the
// endless desert the last reaches 77.8 km from the worst place its window lets the camera stand,
// past where flat ground meets the planet's horizon line from up to 1.9 km, so the ground runs to
// the horizon and the sky's planet shows below it from no height a flight reaches (five reached
// 38.9 km, and from 2,000 m left a band of the planet's flat colour under the horizon).
tunables::Int far_levels_tunable{
    "renderer.terrain.far_levels", 6, 0, static_cast<i64>(k_max_far_levels),
    "How many coarser levels of ground the world's tiles draw past their outermost ring, each "
    "twice the spacing and the reach of the one inside it (RenderSettings::terrain_far_levels -1)"};

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The world's tile order: x, then z, signed (world.md, "Why this shape").
bool tile_before(const TerrainTile& a, const TerrainTile& b) noexcept {
  return a.x < b.x || (a.x == b.x && a.z < b.z);
}

u64 packed(i32 x, i32 z) noexcept {
  return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(z));
}

TerrainTile tile_of(u64 key, u8 ring) noexcept {
  return TerrainTile{static_cast<i32>(static_cast<u32>(key >> 32)),
                     static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFFu)), ring};
}

// Every tile's level, by its packed coordinates.
using LevelMap = HashMap<u64, u8>;

u8 level_at(const LevelMap& map, i32 x, i32 z) noexcept {
  const u8* level = map.find_value(packed(x, z));
  return level != nullptr ? *level : u8{0};
}

// The coarser of two levels, where 0 is "no tile": the smaller non-zero one.
u8 coarsest(u8 a, u8 b) noexcept {
  if (a == 0) return b;
  if (b == 0) return a;
  return std::min(a, b);
}

}  // namespace

u32 terrain_tile_cells(const TerrainTilesDesc& desc, u32 ring) noexcept {
  if (ring < k_max_tile_rings && desc.cells[ring] != 0) return desc.cells[ring];
  return std::max<u32>(8u, 64u >> std::min<u32>(ring, 3u));
}

u32 terrain_far_levels_default() noexcept { return static_cast<u32>(far_levels_tunable.get()); }

TerrainTilesDesc terrain_tiles_desc(const WorldDesc& world, i32 far_levels) noexcept {
  TerrainTilesDesc t;
  t.far_levels = far_levels < 0 ? terrain_far_levels_default()
                                : std::min<u32>(static_cast<u32>(far_levels), k_max_far_levels);
  if (!world.enabled) return t;
  t.tile_size = world.tile_size;
  t.hysteresis = world.hysteresis;
  if (world.ring_count > 0) {
    t.ring_count = std::min<u32>(world.ring_count, k_max_tile_rings);
    for (u32 r = 0; r < k_max_tile_rings; ++r) {
      t.radius[r] = r < t.ring_count ? world.radius[r] : 0.0f;
      t.cells[r] = r < t.ring_count ? world.ground_cells[r] : 0u;
    }
  }
  return t;
}

bool validate_terrain_tiles(const TerrainTilesDesc& desc, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = "world tiles: " + std::move(sentence);
    return false;
  };
  const f64 tile_mm_f = static_cast<f64>(desc.tile_size) * 1000.0;
  const i64 tile_mm = std::llround(tile_mm_f);
  if (!(desc.tile_size > 0.0f) || std::abs(tile_mm_f - static_cast<f64>(tile_mm)) > 1.0e-3)
    return fail("the tile's edge must be a positive whole number of millimetres");
  if (desc.ring_count < 1 || desc.ring_count > k_max_tile_rings)
    return fail("between 1 and " + std::to_string(k_max_tile_rings) + " rings");
  if (!(desc.hysteresis >= 0.0f)) return fail("the hysteresis must not be negative");
  for (u32 r = 0; r < desc.ring_count; ++r) {
    const std::string where = "ring " + std::to_string(r);
    if (!(desc.radius[r] > 0.0f) || (r > 0 && !(desc.radius[r] > desc.radius[r - 1])))
      return fail(where + ": every radius must be positive and larger than the one inside it");
    const u32 cells = terrain_tile_cells(desc, r);
    if (cells < 1 || tile_mm % static_cast<i64>(cells) != 0)
      return fail(where + ": " + std::to_string(cells) +
                  " cells a side do not cut the tile into whole millimetres");
    if (r > 0) {
      const u32 inner = terrain_tile_cells(desc, r - 1);
      if (cells > inner || inner % cells != 0) {
        return fail(where + ": its " + std::to_string(cells) + " cells a side must divide the " +
                    std::to_string(inner) +
                    " of the ring inside it, so a coarser tile's lattice is part of a finer one's");
      }
    }
  }
  if (desc.far_levels > k_max_far_levels)
    return fail("at most " + std::to_string(k_max_far_levels) + " far levels");
  if (desc.far_levels > 0) {
    // The first far level's lattice must hold the world tiles' edges (its holes are world tiles)
    // and the outermost ring's lattice must hold its (the ring's edges collapse onto it); a far
    // tile must be whole world tiles, and few enough of them that its hole is a 64-bit mask.
    const i64 outer = tile_mm / static_cast<i64>(terrain_tile_cells(desc, desc.ring_count - 1));
    const i64 first = outer * static_cast<i64>(desc.far_ratio);
    const i64 far_tile = first * static_cast<i64>(desc.far_cells);
    if (desc.far_ratio < 2) return fail("the first far level must be at least twice as coarse");
    if (desc.far_cells < 1 || tile_mm % first != 0 || far_tile % tile_mm != 0 ||
        (far_tile / tile_mm) * (far_tile / tile_mm) > 64) {
      return fail("a far tile of " + std::to_string(desc.far_cells) + " cells " +
                  std::to_string(first) +
                  " mm apart must be at most eight world tiles a side, cut on a lattice the world "
                  "tile's edges are on");
    }
  }
  return true;
}

TerrainFarLevel terrain_far_level(const TerrainTilesDesc& desc, u32 k) noexcept {
  TerrainFarLevel f;
  if (k == 0 || k > desc.far_levels || desc.ring_count == 0) return f;
  const i64 tile = std::llround(static_cast<f64>(desc.tile_size) * 1000.0);
  const i64 outer = tile / static_cast<i64>(terrain_tile_cells(desc, desc.ring_count - 1));
  const i64 first = outer * static_cast<i64>(desc.far_ratio);
  const i64 first_tile = first * static_cast<i64>(desc.far_cells);
  const i64 first_snap = 2 * first_tile;
  // The first level's half-side: every tile the rings can hold — the outermost radius past its
  // hysteresis, a tile's half-diagonal, and a tile beyond for the neighbours its border is drawn
  // against — seen from a centre as far from the camera as a margin (a quarter of the half-side)
  // and half a snap; and at least six of the next level's tiles, which keeps every level's square
  // inside the next one's wherever the camera is (a centre is at most a margin and half a snap from
  // the camera, so two centres are at most 3/4 of the finer half-side and 1.5 coarser tiles apart).
  const f64 reach = (static_cast<f64>(desc.radius[desc.ring_count - 1]) *
                         (1.0 + static_cast<f64>(desc.hysteresis)) +
                     0.75 + 1.0) *
                    static_cast<f64>(tile);
  const f64 need = (reach + static_cast<f64>(first_snap) / 2.0) / 0.75;
  i64 first_half = static_cast<i64>(std::ceil(need / static_cast<f64>(first_snap))) * first_snap;
  first_half = std::max(first_half, 6 * first_snap);
  f.spacing = first << (k - 1);
  f.tile = first_tile << (k - 1);
  f.snap = first_snap << (k - 1);
  f.half = first_half << (k - 1);
  f.margin = f.half / 4;
  return f;
}

f64 terrain_far_reach_m(const TerrainTilesDesc& desc) noexcept {
  if (desc.far_levels == 0 || desc.ring_count == 0)
    return static_cast<f64>(desc.radius[desc.ring_count == 0 ? 0 : desc.ring_count - 1]) *
           static_cast<f64>(desc.tile_size);
  const TerrainFarLevel f = terrain_far_level(desc, desc.far_levels);
  return static_cast<f64>(f.half - f.margin - f.snap / 2) / 1000.0;
}

void terrain_tiles_round(const TerrainTilesDesc& desc, WorldPos camera, Vector<TerrainTile>& out) {
  out.clear();
  if (desc.ring_count == 0) return;
  const f64 t = static_cast<f64>(desc.tile_size);
  const f64 reach = static_cast<f64>(desc.radius[desc.ring_count - 1]) * t;
  const i64 x0 = static_cast<i64>(std::floor((camera.x - reach) / t)) - 1;
  const i64 x1 = static_cast<i64>(std::floor((camera.x + reach) / t)) + 1;
  const i64 z0 = static_cast<i64>(std::floor((camera.z - reach) / t)) - 1;
  const i64 z1 = static_cast<i64>(std::floor((camera.z + reach) / t)) + 1;
  for (i64 i = x0; i <= x1; ++i) {
    for (i64 j = z0; j <= z1; ++j) {
      // The centre from the camera: the tile's corner is whole tiles, exact in f64 to 1e15 m.
      const f64 dx = (static_cast<f64>(i) + 0.5) * t - camera.x;
      const f64 dz = (static_cast<f64>(j) + 0.5) * t - camera.z;
      const f64 d = std::sqrt(dx * dx + dz * dz);
      for (u32 r = 0; r < desc.ring_count; ++r) {
        if (d < static_cast<f64>(desc.radius[r]) * t) {
          out.push_back(TerrainTile{static_cast<i32>(i), static_cast<i32>(j), static_cast<u8>(r)});
          break;
        }
      }
    }
  }
}

// ---- a tile's mesh ------------------------------------------------------------------------------

u64 terrain_tile_key(const TerrainTileMeshSpec& spec) noexcept {
  u64 h = hash_combine(packed(spec.x, spec.z), static_cast<u64>(spec.level));
  h = hash_combine(h, static_cast<u64>(spec.cells));
  h = hash_combine(h, static_cast<u64>(spec.spacing_mm));
  // Only what changes the mesh: a neighbour coarser than the tile (its edge is collapsed onto that
  // neighbour's lattice and drawn from its level), and a corner a coarser tile draws.
  for (u32 e = 0; e < 4; ++e) {
    const u8 n = spec.neighbours.edge[e];
    h = hash_combine(h, n != 0 && n < spec.level ? static_cast<u64>(n) : 0u);
  }
  for (u32 c = 0; c < 4; ++c) {
    const u8 n = coarsest(spec.neighbours.corner[c], spec.level);
    h = hash_combine(h, n < spec.level ? static_cast<u64>(n) : 0u);
  }
  // A far tile's hole: the world tiles the rings draw inside it.
  if (spec.hole_mask != 0) h = hash_combine(h, spec.hole_mask);
  return h;
}

void build_terrain_tile_mesh(const TerrainTileMeshSpec& spec, std::span<const f32> heights,
                             TerrainTileMesh& out) {
  const u32 c = spec.cells;
  const u32 n = c + 1;  // vertices a side
  const u32 a = c + 3;  // heights a side, with a point of apron
  const i64 s = spec.spacing_mm;
  const i32 i0 = spec.x * static_cast<i32>(c);
  const i32 j0 = spec.z * static_cast<i32>(c);
  const auto h = [&](i32 i, i32 j) {  // tile-local lattice indices, -1..c+1
    return heights[static_cast<usize>(j + 1) * a + static_cast<usize>(i + 1)];
  };
  // Along each edge: the level a coarser neighbour is drawn at and how many of this tile's cells
  // one of its cells is (1: not collapsed). Edges -x, +x, -z, +z.
  u8 edge_level[4] = {};
  u32 ratio[4] = {1, 1, 1, 1};
  for (u32 e = 0; e < 4; ++e) {
    const u8 nl = spec.neighbours.edge[e];
    if (nl == 0 || nl >= spec.level) continue;
    // How many of this tile's cells one of the coarser lattice's is: by the spacings where the
    // spec gives them (a far level's tiles are larger than the world's), else by the cells of two
    // tiles of one size.
    const i64 ns = spec.level_spacing_mm[nl];
    u32 r = 0;
    if (ns > 0) {
      if (ns % s != 0) continue;
      r = static_cast<u32>(ns / s);
    } else {
      const u32 nc = spec.level_cells[nl];
      if (nc == 0 || c % nc != 0) continue;
      r = c / nc;
    }
    if (r <= 1 || c % r != 0) continue;
    edge_level[e] = nl;
    ratio[e] = r;
  }
  // A far tile's hole: the cells under the world tiles the rings draw.
  const auto hole = [&](u32 i, u32 j) {
    if (spec.hole_mask == 0 || spec.hole_cells == 0) return false;
    const u32 bit = (j / spec.hole_cells) * spec.hole_side + i / spec.hole_cells;
    return ((spec.hole_mask >> bit) & 1u) != 0;
  };
  u8 corner_level[4] = {};
  for (u32 k = 0; k < 4; ++k) {
    const u8 nl = coarsest(spec.neighbours.corner[k], spec.level);
    corner_level[k] = nl < spec.level ? nl : u8{0};
  }
  // Which grid vertex each one is drawn as: itself, or — on an edge along a coarser tile, between
  // two of its lattice points — the nearer of them (the lower on a tie).
  const auto rep = [&](u32 i, u32 j) -> u32 {
    const auto collapse = [](u32 k, u32 r) {
      const u32 k0 = k / r * r;
      return k - k0 <= r / 2 ? k0 : k0 + r;
    };
    if (j == 0 && ratio[2] > 1) i = collapse(i, ratio[2]);
    if (j == c && ratio[3] > 1) i = collapse(i, ratio[3]);
    if (i == 0 && ratio[0] > 1) j = collapse(j, ratio[0]);
    if (i == c && ratio[1] > 1) j = collapse(j, ratio[1]);
    return j * n + i;
  };
  // The triangles, two counter-clockwise a cell with the scene grid's diagonal (`build_terrain_
  // mesh`: a, c, b and b, c, d), every corner drawn as its representative; the ones that leaves
  // with two corners at one vertex are dropped.
  Vector<u32> tri;
  tri.reserve(static_cast<usize>(c) * c * 6);
  for (u32 j = 0; j < c; ++j) {
    for (u32 i = 0; i < c; ++i) {
      if (hole(i, j)) continue;
      const u32 va = rep(i, j);
      const u32 vb = rep(i + 1, j);
      const u32 vc = rep(i, j + 1);
      const u32 vd = rep(i + 1, j + 1);
      // **The corner cell of two collapsed edges** (2026-10-03, found by the far levels' tiling
      // test): b and c are pulled to the lattice points either side of the corner, so a, c, b lie
      // on one line — a triangle of no area — and a is on b d c's long edge, a T-junction. The
      // cell's other diagonal splits it into two triangles that meet the collapsed edges' fans.
      const i64 ax = va % n, az = va / n;
      const i64 cross = (static_cast<i64>(vc % n) - ax) * (static_cast<i64>(vb / n) - az) -
                        (static_cast<i64>(vc / n) - az) * (static_cast<i64>(vb % n) - ax);
      if (va != vb && va != vc && vb != vc && vd != va && cross == 0) {
        if (vc != vd) {
          tri.push_back(va);
          tri.push_back(vc);
          tri.push_back(vd);
        }
        if (vd != vb) {
          tri.push_back(va);
          tri.push_back(vd);
          tri.push_back(vb);
        }
        continue;
      }
      if (va != vc && vc != vb && va != vb) {
        tri.push_back(va);
        tri.push_back(vc);
        tri.push_back(vb);
      }
      if (vb != vc && vc != vd && vb != vd) {
        tri.push_back(vb);
        tri.push_back(vc);
        tri.push_back(vd);
      }
    }
  }
  // The vertices the triangles use, in grid order.
  Vector<u32> slot(static_cast<usize>(n) * n, ~0u);
  for (const u32 v : tri)
    slot[v] = 0;
  out.positions.clear();
  out.normals.clear();
  out.uvs.clear();
  out.locked.clear();
  out.indices.clear();
  out.lattice_i.clear();
  out.lattice_j.clear();
  out.drawn_from.clear();
  out.corner_x_mm = static_cast<i64>(i0) * s;
  out.corner_z_mm = static_cast<i64>(j0) * s;
  const f64 uv_scale = 1.0 / static_cast<f64>(spec.uv_size_mm);
  const f64 spacing_m = static_cast<f64>(s) / 1000.0;
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      u32& v = slot[j * n + i];
      if (v == ~0u) continue;
      v = out.positions.size();
      const i32 li = i0 + static_cast<i32>(i);
      const i32 lj = j0 + static_cast<i32>(j);
      const i64 xm = static_cast<i64>(li) * s;
      const i64 zm = static_cast<i64>(lj) * s;
      const f32 y = h(static_cast<i32>(i), static_cast<i32>(j));
      // From the tile's corner (renderer.md, "The ground's tiles are placed at their corners"):
      // whole millimetres, rounded to float32 once at the size of the tile.
      out.positions.push_back(
          Vec3{static_cast<f32>(static_cast<f64>(xm - out.corner_x_mm) / 1000.0), y,
               static_cast<f32>(static_cast<f64>(zm - out.corner_z_mm) / 1000.0)});
      out.uvs.push_back(Vec2{static_cast<f32>(static_cast<f64>(xm - spec.uv_x0_mm) * uv_scale),
                             static_cast<f32>(static_cast<f64>(zm - spec.uv_z0_mm) * uv_scale)});
      // The border, and a far tile's hole's: every DAG level keeps the vertices a finer tile meets.
      bool border = i == 0 || j == 0 || i == c || j == c;
      for (u32 dj = 0; !border && dj < 2; ++dj) {
        for (u32 di = 0; !border && di < 2; ++di)
          border = hole(i + di - 1, j + dj - 1);
      }
      out.locked.push_back(border ? u8{1} : u8{0});
      out.lattice_i.push_back(li);
      out.lattice_j.push_back(lj);
      // Which level draws it: a corner the coarsest tile round it, an edge along a coarser tile
      // that tile, anything else this tile. A heightfield normal by central differences over the
      // apron, or the drawing level named in a horizontal one.
      u8 from = spec.level;
      const bool on_x0 = i == 0, on_x1 = i == c, on_z0 = j == 0, on_z1 = j == c;
      if ((on_x0 || on_x1) && (on_z0 || on_z1)) {
        const u8 k = corner_level[(on_x1 ? 1u : 0u) + (on_z1 ? 2u : 0u)];
        if (k != 0) from = k;
      } else if (on_x0 && edge_level[0] != 0) {
        from = edge_level[0];
      } else if (on_x1 && edge_level[1] != 0) {
        from = edge_level[1];
      } else if (on_z0 && edge_level[2] != 0) {
        from = edge_level[2];
      } else if (on_z1 && edge_level[3] != 0) {
        from = edge_level[3];
      }
      out.drawn_from.push_back(from);
      if (from != spec.level) {
        out.normals.push_back(gfx::terrain_level_normal(from));
      } else {
        const i32 ii = static_cast<i32>(i);
        const i32 jj = static_cast<i32>(j);
        const f64 dx = static_cast<f64>(h(ii + 1, jj) - h(ii - 1, jj)) / (2.0 * spacing_m);
        const f64 dz = static_cast<f64>(h(ii, jj + 1) - h(ii, jj - 1)) / (2.0 * spacing_m);
        const f64 len = std::sqrt(dx * dx + 1.0 + dz * dz);
        out.normals.push_back(Vec3{static_cast<f32>(-dx / len), static_cast<f32>(1.0 / len),
                                   static_cast<f32>(-dz / len)});
      }
    }
  }
  out.indices.reserve(tri.size());
  for (const u32 v : tri)
    out.indices.push_back(slot[v]);
}

// ---- the set ------------------------------------------------------------------------------------

TerrainTileSet::TerrainTileSet() = default;
TerrainTileSet::~TerrainTileSet() = default;

Vec4 TerrainTileSet::grid_hole(const TerrainRingLayout&) const noexcept {
  // The whole world: every cluster of the grid is wholly inside and culled.
  constexpr f32 k_far = 1.0e9f;
  return Vec4{-k_far, -k_far, k_far, k_far};
}

TerrainRingLayout TerrainTileSet::layout() const noexcept { return layout_; }

void TerrainTileSet::window_centre(u32 level, WorldPos camera, i64& cx, i64& cz) const noexcept {
  if (is_far(level)) {
    // On the next coarser level's tile grid, nearest the camera, so the square's border is on that
    // level's lattice and the square it leaves out of it is whole tiles of it.
    const f64 snap = static_cast<f64>(far_snap_mm_[level]) / 1000.0;
    cx = static_cast<i64>(std::floor(camera.x / snap + 0.5)) * far_snap_mm_[level];
    cz = static_cast<i64>(std::floor(camera.z / snap + 0.5)) * far_snap_mm_[level];
    return;
  }
  const f64 t = static_cast<f64>(tile_mm_) / 1000.0;
  cx = static_cast<i64>(std::floor(camera.x / t)) * tile_mm_;
  cz = static_cast<i64>(std::floor(camera.z / t)) * tile_mm_;
}

u8 TerrainTileSet::far_level_at(const TerrainRingLayout& layout, i32 x, i32 z) const noexcept {
  const i64 x0 = static_cast<i64>(x) * tile_mm_;
  const i64 z0 = static_cast<i64>(z) * tile_mm_;
  for (u32 level = far_; level >= 1; --level) {
    const i64 half = layout.half[level];
    if (x0 >= layout.cx[level] - half && x0 + tile_mm_ <= layout.cx[level] + half &&
        z0 >= layout.cz[level] - half && z0 + tile_mm_ <= layout.cz[level] + half)
      return static_cast<u8>(level);
  }
  return 0;
}

bool TerrainTileSet::far_tile(u32 level, const TerrainRingLayout& layout,
                              const HashMap<u64, u8>& level_of, i32 x, i32 z, FarWant& out) const {
  const i64 t = far_tile_mm_[level];
  const i64 x0 = static_cast<i64>(x) * t;
  const i64 z0 = static_cast<i64>(z) * t;
  const i64 cx = layout.cx[level];
  const i64 cz = layout.cz[level];
  const i64 half = layout.half[level];
  if (x0 < cx - half || x0 + t > cx + half || z0 < cz - half || z0 + t > cz + half) return false;
  TerrainTileMeshSpec& spec = out.spec;
  spec = TerrainTileMeshSpec{};
  if (level < far_) {
    // Inside the next finer far level's square: that level's.
    const u32 f = level + 1;
    const i64 fh = layout.half[f];
    if (x0 >= layout.cx[f] - fh && x0 + t <= layout.cx[f] + fh && z0 >= layout.cz[f] - fh &&
        z0 + t <= layout.cz[f] + fh)
      return false;
  } else {
    // The finest: the world tiles the rings draw inside it are its hole.
    const u32 side = static_cast<u32>(t / tile_mm_);
    u64 mask = 0;
    for (u32 b = 0; b < side; ++b) {
      for (u32 a = 0; a < side; ++a) {
        const i32 wx = x * static_cast<i32>(side) + static_cast<i32>(a);
        const i32 wz = z * static_cast<i32>(side) + static_cast<i32>(b);
        if (level_of.find_value(packed(wx, wz)) != nullptr) mask |= u64{1} << (b * side + a);
      }
    }
    if (side * side == 64 ? mask == ~u64{0} : mask == (u64{1} << (side * side)) - 1) return false;
    spec.hole_mask = mask;
    spec.hole_side = side;
    spec.hole_cells = static_cast<u32>(tile_mm_ / lattice_[level].spacing_mm);
  }
  spec.x = x;
  spec.z = z;
  spec.level = static_cast<u8>(level);
  spec.cells = level_cells_[level];
  spec.spacing_mm = lattice_[level].spacing_mm;
  for (u32 l = 0; l < levels_; ++l) {
    spec.level_cells[l] = level_cells_[l];
    spec.level_spacing_mm[l] = l == 0 ? 0 : lattice_[l].spacing_mm;
  }
  spec.uv_x0_mm = uv_corner_mm_;
  spec.uv_z0_mm = uv_corner_mm_;
  spec.uv_size_mm = uv_size_mm_;
  // Its neighbours: this level inside its square, the next coarser far level past its border (none
  // past the coarsest's), and whatever is finer inside — which does not change its mesh.
  const u8 outside = static_cast<u8>(level - 1);
  const bool west = x0 == cx - half;
  const bool east = x0 + t == cx + half;
  const bool south = z0 == cz - half;
  const bool north = z0 + t == cz + half;
  const u8 own = static_cast<u8>(level);
  spec.neighbours.edge[0] = west ? outside : own;
  spec.neighbours.edge[1] = east ? outside : own;
  spec.neighbours.edge[2] = south ? outside : own;
  spec.neighbours.edge[3] = north ? outside : own;
  spec.neighbours.corner[0] = west || south ? outside : own;
  spec.neighbours.corner[1] = east || south ? outside : own;
  spec.neighbours.corner[2] = west || north ? outside : own;
  spec.neighbours.corner[3] = east || north ? outside : own;
  out.key = terrain_tile_key(spec);
  return true;
}

bool TerrainTileSet::chunk_spec(u32 level, u32 index, TerrainTileMeshSpec& out) const {
  if (level == 0 || level >= levels_ || index >= chunks_[level].size()) return false;
  const TerrainChunk& chunk = chunks_[level][index];
  if (is_far(level)) {
    FarWant w;
    if (!far_tile(level, layout_, level_of_, chunk.i, chunk.j, w)) return false;
    out = w.spec;
    return true;
  }
  out = spec_of(chunk.i, chunk.j, static_cast<u8>(level), level_of_, layout_);
  return true;
}

void TerrainTileSet::far_tiles(u32 level, const TerrainRingLayout& layout,
                               const HashMap<u64, u8>& level_of, Vector<FarWant>& out) const {
  out.clear();
  const i64 t = far_tile_mm_[level];
  const i64 half = layout.half[level];
  const i32 x0 = static_cast<i32>(floor_div(layout.cx[level] - half, t));
  const i32 x1 = static_cast<i32>(floor_div(layout.cx[level] + half, t));
  const i32 z0 = static_cast<i32>(floor_div(layout.cz[level] - half, t));
  const i32 z1 = static_cast<i32>(floor_div(layout.cz[level] + half, t));
  FarWant w;
  for (i32 x = x0; x < x1; ++x) {
    for (i32 z = z0; z < z1; ++z) {
      if (far_tile(level, layout, level_of, x, z, w)) out.push_back(w);
    }
  }
}

gfx::TerrainField TerrainTileSet::field_window(u32 level,
                                               const TerrainRingLayout& layout) const noexcept {
  gfx::TerrainField w{};
  if (level == 0 || level >= levels_) {
    w.nx = 1;
    w.nz = 1;
    return w;
  }
  const i64 s = lattice_[level].spacing_mm;
  const i64 half = layout.half[level];
  w.i0 = static_cast<i32>(floor_div(layout.cx[level] - half, s) - 1);
  w.j0 = static_cast<i32>(floor_div(layout.cz[level] - half, s) - 1);
  w.nx = static_cast<u32>(2 * half / s + 3);
  w.nz = w.nx;
  return w;
}

u64 TerrainTileSet::field_capacity(u32 level) const noexcept {
  if (level == 0) return 1;
  if (level >= levels_) return 0;
  const u64 side = static_cast<u64>(2 * half_mm_[level] / lattice_[level].spacing_mm + 3);
  return side * side;
}

bool TerrainTileSet::covers(u32 level, const TerrainRingLayout& layout, i32 x,
                            i32 z) const noexcept {
  // The tile's square, and a point of apron round it for its normals, inside the window's square.
  const i64 half = layout.half[level];
  const i64 x0 = static_cast<i64>(x) * tile_mm_;
  const i64 z0 = static_cast<i64>(z) * tile_mm_;
  return x0 >= layout.cx[level] - half && x0 + tile_mm_ <= layout.cx[level] + half &&
         z0 >= layout.cz[level] - half && z0 + tile_mm_ <= layout.cz[level] + half;
}

bool TerrainTileSet::inside_far(const TerrainRingLayout& layout, i32 x, i32 z) const noexcept {
  if (far_ == 0) return true;
  // With far levels, a ring's tile is drawn only a tile inside the finest far level's square, so
  // every tile round it is drawn — a ring's or that far level's — and its border has something
  // to meet. A world whose ring holds tiles past that (a budget behind a fast camera) has them
  // drawn by the far levels until the square comes to them.
  const i64 x0 = static_cast<i64>(x) * tile_mm_;
  const i64 z0 = static_cast<i64>(z) * tile_mm_;
  const i64 fh = layout.half[far_];
  return x0 - tile_mm_ >= layout.cx[far_] - fh && x0 + 2 * tile_mm_ <= layout.cx[far_] + fh &&
         z0 - tile_mm_ >= layout.cz[far_] - fh && z0 + 2 * tile_mm_ <= layout.cz[far_] + fh;
}

TerrainRingLayout TerrainTileSet::next_layout(WorldPos camera,
                                              const TerrainRingLayout& from) const noexcept {
  TerrainRingLayout out = from;
  if (!valid()) return out;
  out.generation = generation_;
  for (u32 level = 1; level < levels_; ++level) {
    i64 cx = 0;
    i64 cz = 0;
    window_centre(level, camera, cx, cz);
    if (is_far(level)) {
      // A far level's square follows the camera by the same margin rule, on its snap; nothing the
      // world holds moves it (its finest level's square holds every tile the rings can).
      if (std::max(std::abs(cx - from.cx[level]), std::abs(cz - from.cz[level])) >
              margin_mm_[level] ||
          from.half[level] != half_mm_[level]) {
        out.cx[level] = cx;
        out.cz[level] = cz;
      }
      out.half[level] = half_mm_[level];
      continue;
    }
    // The camera has gone more than the margin from the window's centre, or a tile the world holds
    // at this level is outside it: the window moves to the camera.
    bool move =
        std::max(std::abs(cx - from.cx[level]), std::abs(cz - from.cz[level])) > margin_mm_[level];
    // What the world holds outside it: the tiles the last rebuild withheld for its window, and the
    // changes since — not every tile held, which this asked every frame until 2026-10-03.
    if (!move && from == layout_ && window_withheld_[level] > 0) move = true;
    for (u32 k = 0; !move && k < changes_.size(); ++k) {
      const TerrainTile& t = changes_[k];
      move =
          t.ring != k_tile_gone && level_of_ring(t.ring) == level && !covers(level, from, t.x, t.z);
    }
    if (!move && !(from == layout_)) {
      // Asked of another layout than the one built last (a test's): every tile held.
      for (u32 k = 0; !move && k < held_.size(); ++k) {
        const TerrainTile t = tile_of(held_.key_at(k), held_.value_at(k));
        move = level_of_ring(t.ring) == level && !covers(level, from, t.x, t.z);
      }
    }
    if (move) {
      out.cx[level] = cx;
      out.cz[level] = cz;
    }
    out.half[level] = half_mm_[level];
  }
  return out;
}

bool TerrainTileSet::set_tiles(std::span<const TerrainTile> tiles) {
  // The whole set: what differs from the tiles held becomes changes (a tile named twice keeps its
  // first ring), and a tile held and not named is let go. Linear in the set; a world's ring hands
  // its changes instead (`change_tiles`), which cost what changed.
  incoming_.clear();
  incoming_.reserve(static_cast<u32>(tiles.size()));
  const usize before = changes_.size();
  for (const TerrainTile& t : tiles) {
    const u64 key = packed(t.x, t.z);
    if (incoming_.find_value(key) != nullptr) continue;
    incoming_.insert(key, t.ring);
    const u8* held = held_.find_value(key);
    if (held != nullptr && *held == t.ring) continue;
    held_.insert_or_assign(key, t.ring);
    changes_.push_back(t);
  }
  for (u32 k = 0; k < held_.size();) {
    const u64 key = held_.key_at(k);
    if (incoming_.find_value(key) != nullptr) {
      ++k;
      continue;
    }
    changes_.push_back(tile_of(key, k_tile_gone));
    held_.erase(key);  // the map compacts: the next key is at k
  }
  if (changes_.size() == before) return false;
  ++generation_;
  return true;
}

bool TerrainTileSet::change_tiles(std::span<const TerrainTile> changes) {
  bool changed = false;
  for (const TerrainTile& t : changes) {
    const u64 key = packed(t.x, t.z);
    const u8* held = held_.find_value(key);
    if (t.ring == k_tile_gone) {
      if (held == nullptr) continue;
      held_.erase(key);
    } else {
      if (held != nullptr && *held == t.ring) continue;
      held_.insert_or_assign(key, t.ring);
    }
    changes_.push_back(t);
    changed = true;
  }
  if (changed) ++generation_;
  return changed;
}

void TerrainTileSet::prepare(const TerrainRingLayout& target) {
  (void)target;
  // The changes since the last rebuild are the worker's from here; both lists keep their room.
  std::swap(taken_, changes_);
  changes_.clear();
}

bool TerrainTileSet::find(i32 x, i32 z, u32& level, u32& index) const noexcept {
  const u32* at = chunk_at_.find_value(packed(x, z));
  if (at == nullptr) return false;
  level = *at >> 24;
  index = *at & 0xFFFFFFu;
  return true;
}

std::span<const u32> TerrainTileSet::added(u32 level) const noexcept {
  if (level >= k_max_terrain_levels) return {};
  return std::span<const u32>(added_[level].data(), added_[level].size());
}

std::span<const u32> TerrainTileSet::released(u32 level) const noexcept {
  if (level >= k_max_terrain_levels) return {};
  return std::span<const u32>(released_[level].data(), released_[level].size());
}

f64 TerrainTileSet::padding_added(u32 level, std::span<const f32> field,
                                  const gfx::TerrainField& window) const {
  if (level == 0 || level >= levels_) return 0.0;
  std::lock_guard<std::mutex> lock(mutex_);
  f64 most = 0.0;
  for (const u32 c : added_[level]) {
    if (c >= chunks_[level].size()) continue;
    most = std::max(
        most, padding_of(std::span<const TerrainChunk>(&chunks_[level][c], 1), field, window));
  }
  return most;
}

// The incremental model from the chunk lists as they stand (after the first fill or a whole
// rebuild): every drawn tile's level and place, every held tile's ring, the window's withholdings.
void TerrainTileSet::index_chunks() {
  level_of_.clear();
  chunk_at_.clear();
  for (u32 level = 1; level < levels_; ++level) {
    count_[level] = chunks_[level].size();
    if (is_far(level)) {
      far_at_[level].clear();
      for (u32 c = 0; c < chunks_[level].size(); ++c)
        far_at_[level].insert(packed(chunks_[level][c].i, chunks_[level][c].j), c);
      continue;
    }
    for (u32 c = 0; c < chunks_[level].size(); ++c) {
      const u64 key = packed(chunks_[level][c].i, chunks_[level][c].j);
      level_of_.insert(key, static_cast<u8>(level));
      chunk_at_.insert(key, (level << 24) | c);
    }
  }
  built_ring_.clear();
  for (u32 k = 0; k < building_.size(); ++k)
    built_ring_.insert(packed(building_[k].x, building_[k].z), building_[k].ring);
}

f64 TerrainTileSet::padding(u32 level, std::span<const f32> field,
                            const gfx::TerrainField& window) const {
  if (level == 0 || level >= levels_) return 0.0;
  std::lock_guard<std::mutex> lock(mutex_);
  const f64 now = padding_of(
      std::span<const TerrainChunk>(chunks_[level].data(), chunks_[level].size()), field, window);
  const f64 before =
      padding_of(std::span<const TerrainChunk>(previous_[level].data(), previous_[level].size()),
                 field, window);
  return std::max(now, before);
}

bool TerrainTileSet::build(const TerrainDesc& terrain, const TerrainTilesDesc& tiles,
                           const scene_gen::TileSource& source, WorldPos camera,
                           jobs::JobSystem* jobs, std::string* error) {
  levels_ = 0;
  for (u32 l = 0; l < k_max_terrain_levels; ++l) {
    chunks_[l].clear();
    previous_[l].clear();
    capacity_[l] = Capacity{};
  }
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (!terrain.enabled) return fail("world tiles: the scene has no terrain");
  if (!validate_terrain_tiles(tiles, error)) return false;
  if (!source.valid()) return fail("world tiles: the tile source has no heights");
  const i64 extent = std::llround(static_cast<f64>(terrain.extent) * 1000.0);
  if (std::abs(static_cast<f64>(terrain.extent) * 1000.0 - static_cast<f64>(extent)) > 1.0e-3 ||
      extent <= 0) {
    return fail(
        "world tiles: the scene grid's half-side must be whole millimetres: the tiles' UVs are in "
        "its frame");
  }
  const i64 started = time::monotonic_ns();
  desc_ = &terrain;
  tiles_ = tiles;
  source_ = source;
  options_ = geometry::ClusterLodOptions{};
  tile_mm_ = std::llround(static_cast<f64>(tiles.tile_size) * 1000.0);
  uv_corner_mm_ = -extent;
  uv_size_mm_ = 2 * extent;
  far_ = tiles.far_levels;
  levels_ = far_ + tiles.ring_count + 1;
  lattice_[0] = terrain_scene_lattice(terrain);
  level_cells_[0] = 0;
  const f64 t = static_cast<f64>(tiles.tile_size);
  for (u32 level = 1; level <= far_; ++level) {
    // A far level: its lattice filtered to its spacing (the source answers with what it can
    // carry), its square and the snap its centre stands on.
    const TerrainFarLevel f = terrain_far_level(tiles, far_index(level));
    level_cells_[level] = tiles.far_cells;
    lattice_[level] = terrain_ring_lattice(f.spacing, f.spacing);
    half_mm_[level] = f.half;
    margin_mm_[level] = f.margin;
    far_tile_mm_[level] = f.tile;
    far_snap_mm_[level] = f.snap;
    far_at_[level].clear();
  }
  for (u32 level = far_ + 1; level < levels_; ++level) {
    const u32 ring = ring_of_level(level);
    level_cells_[level] = terrain_tile_cells(tiles, ring);
    lattice_[level] = terrain_ring_lattice(tile_mm_ / static_cast<i64>(level_cells_[level]));
    // The window: every tile the ring can hold with the camera within the margin of the centre (a
    // tile's centre within the radius past its hysteresis, the tile's half beyond that), rounded
    // out to whole tiles.
    const f64 margin_tiles = std::max(2.0, static_cast<f64>(tiles.radius[ring]) / 4.0);
    const f64 reach_tiles =
        static_cast<f64>(tiles.radius[ring]) * (1.0 + static_cast<f64>(tiles.hysteresis)) + 0.5 +
        margin_tiles;
    margin_mm_[level] = std::llround(margin_tiles * t * 1000.0);
    half_mm_[level] = static_cast<i64>(std::ceil(reach_tiles)) * tile_mm_;
  }
  // The first layout: every window on the camera's tile, the world ring's first fill round it.
  layout_ = TerrainRingLayout{};
  for (u32 level = 1; level < levels_; ++level) {
    window_centre(level, camera, layout_.cx[level], layout_.cz[level]);
    layout_.half[level] = half_mm_[level];
  }
  Vector<TerrainTile> first;
  terrain_tiles_round(tiles, camera, first);
  held_.clear();
  changes_.clear();
  taken_.clear();
  generation_ = 0;
  set_tiles(std::span<const TerrainTile>(first.data(), first.size()));
  changes_.clear();  // the first fill is built whole, below
  layout_.generation = generation_;
  building_.assign(first.begin(), first.end());
  u32 moved = 0;
  if (!rebuild(layout_, terrain.time_s, jobs, moved, error)) {
    levels_ = 0;
    return false;
  }
  index_chunks();
  for (u32 level = 1; level < levels_; ++level)
    previous_[level].clear();
  // What the GPU scene reserves a level: two slots for every tile its ring can hold (one drawn and
  // one replacing it), counted with the camera anywhere in its tile, each slot and the arenas room
  // for the largest tile built here with `renderer.terrain.tile_slack` on top.
  const f64 slack = tile_slack.get();
  for (u32 level = 1; level < levels_; ++level) {
    const bool far = is_far(level);
    const u32 ring = far ? 0u : ring_of_level(level);
    u64 most_tiles = 0;
    if (far) {
      // Its square's tiles, less the square of the next finer far level (the finest's hole is
      // the rings' tiles, which it is not counted short by: a hole is a mask, not a tile less).
      const u64 side = static_cast<u64>(2 * half_mm_[level] / far_tile_mm_[level]);
      most_tiles = side * side - (level < far_ ? (side / 2) * (side / 2) : 0u);
    } else {
      const f64 reach =
          static_cast<f64>(tiles.radius[ring]) * (1.0 + static_cast<f64>(tiles.hysteresis)) +
          0.7072;
      const i32 span = static_cast<i32>(std::ceil(reach)) + 1;
      for (i32 i = -span; i <= span; ++i) {
        for (i32 j = -span; j <= span; ++j) {
          const f64 d = std::hypot(static_cast<f64>(i), static_cast<f64>(j));
          if (d <= reach) ++most_tiles;
        }
      }
    }
    u32 clusters = 0;
    u64 vertices = 0;
    u64 triangles = 0;
    for (const TerrainChunk& chunk : chunks_[level]) {
      clusters = std::max<u32>(clusters, chunk.lod.mesh.clusters.size());
      vertices = std::max<u64>(vertices, chunk.lod.mesh.vertices.size());
      triangles = std::max<u64>(triangles, chunk.lod.mesh.triangles.size());
    }
    if (clusters == 0) {
      // No tile of this ring in the first layout (a ring narrower than a tile's half-diagonal):
      // one built where the camera is, for its size alone.
      TerrainTileMeshSpec spec;
      spec.level = static_cast<u8>(level);
      spec.cells = level_cells_[level];
      spec.spacing_mm = lattice_[level].spacing_mm;
      spec.uv_x0_mm = uv_corner_mm_;
      spec.uv_z0_mm = uv_corner_mm_;
      spec.uv_size_mm = uv_size_mm_;
      const u32 a = spec.cells + 3;
      Vector<f32> h(static_cast<usize>(a) * a);
      (void)source_.filtered(terrain.time_s, spec.spacing_mm, lattice_[level].filter_mm, -1, -1, a,
                             a, std::span<f32>(h.data(), h.size()));
      TerrainTileMesh mesh;
      build_terrain_tile_mesh(spec, std::span<const f32>(h.data(), h.size()), mesh);
      geometry::ClusterLodMesh lod;
      geometry::AttributeSource attributes;
      attributes.normals = std::span<const Vec3>(mesh.normals.data(), mesh.normals.size());
      attributes.uvs = std::span<const Vec2>(mesh.uvs.data(), mesh.uvs.size());
      attributes.locked = std::span<const u8>(mesh.locked.data(), mesh.locked.size());
      if (!geometry::build_cluster_lod(
              std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
              std::span<const u32>(mesh.indices.data(), mesh.indices.size()), options_, lod, error,
              attributes)) {
        levels_ = 0;
        return false;
      }
      clusters = lod.mesh.clusters.size();
      vertices = lod.mesh.vertices.size();
      triangles = lod.mesh.triangles.size();
    }
    Capacity& c = capacity_[level];
    c.slots = static_cast<u32>(2 * most_tiles);
    // A slot holds the largest tile built here with the slack on top, and never fewer clusters than
    // a tile that did not simplify at all could have (its grid's triangles in clusters of at least
    // 64, doubled for the DAG's levels above its leaves): a tile over rougher sand than any the
    // first layout held must not find its slot too small. Rounded to four, not to the sixteen it
    // was: a slot's run is pairs the cull pass visits every frame whether the tile fills it or
    // not, and a coarse ring's tile is one or two clusters, so sixteen was eight times its pairs
    // over the ring with the most slots.
    const u32 grid_triangles = 2 * level_cells_[level] * level_cells_[level];
    const u32 unsimplified = 2 * ((grid_triangles + 63) / 64);
    c.clusters_per_slot =
        (std::max(static_cast<u32>(std::ceil(static_cast<f64>(clusters) * slack)), unsimplified) +
         3u) /
        4u * 4u;
    c.vertices = static_cast<u64>(static_cast<f64>(vertices) * slack) * c.slots + 1024;
    c.triangles = static_cast<u64>(static_cast<f64>(triangles) * slack) * c.slots + 1024;
    ENGINE_LOG_INFO(
        log_renderer, far ? "far tile level" : "world tile level", log::field("level", level),
        log::field("ring", far ? -static_cast<i64>(far_index(level)) : static_cast<i64>(ring)),
        log::field("cells", level_cells_[level]), log::field("spacing_m", lattice_[level].spacing),
        log::field("tiles", chunks_[level].size()), log::field("slots", c.slots),
        log::field("clusters_per_slot", c.clusters_per_slot), log::field("vertices", c.vertices),
        log::field("triangles", c.triangles),
        log::field("window_m", 2.0 * static_cast<f64>(half_mm_[level]) / 1000.0));
  }
  last_build_ms_ = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  return true;
}

bool TerrainTileSet::update(WorldPos camera, f64 time_s, const TerrainRingLayout& target,
                            std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
                            std::string* error) {
  (void)camera;
  (void)fields;
  moved = 0;
  if (!valid()) return true;
  const i64 started = time::monotonic_ns();
  if (!rebuild_changes(target, time_s, jobs, moved, error)) return false;
  last_build_ms_ = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  return true;
}

TerrainTileMeshSpec TerrainTileSet::spec_of(i32 x, i32 z, u8 level,
                                            const HashMap<u64, u8>& level_of,
                                            const TerrainRingLayout& layout) const noexcept {
  TerrainTileMeshSpec spec;
  spec.x = x;
  spec.z = z;
  spec.level = level;
  spec.cells = level_cells_[level];
  spec.spacing_mm = lattice_[level].spacing_mm;
  for (u32 l = 0; l < levels_; ++l) {
    spec.level_cells[l] = level_cells_[l];
    spec.level_spacing_mm[l] = l == 0 ? 0 : lattice_[l].spacing_mm;
  }
  spec.uv_x0_mm = uv_corner_mm_;
  spec.uv_z0_mm = uv_corner_mm_;
  spec.uv_size_mm = uv_size_mm_;
  // A tile no ring draws is the far level's whose square holds it (none without far levels).
  const auto at = [&](i32 i, i32 j) {
    const u8 l = level_at(level_of, i, j);
    return l != 0 || far_ == 0 ? l : far_level_at(layout, i, j);
  };
  spec.neighbours.edge[0] = at(x - 1, z);
  spec.neighbours.edge[1] = at(x + 1, z);
  spec.neighbours.edge[2] = at(x, z - 1);
  spec.neighbours.edge[3] = at(x, z + 1);
  const i32 dx[4] = {-1, 1, -1, 1};
  const i32 dz[4] = {-1, -1, 1, 1};
  for (u32 c = 0; c < 4; ++c) {
    spec.neighbours.corner[c] =
        coarsest(coarsest(at(x + dx[c], z), at(x, z + dz[c])), at(x + dx[c], z + dz[c]));
  }
  return spec;
}

// **A rebuild that costs what changed** (renderer.md, "What a frame waits for"): the world's
// changes since the last one, and the tiles of each level whose window moved, are the tiles whose
// drawn level may change; those that did change, and their eight neighbours (whose keys name their
// levels), are the only tiles whose keys are asked again, and of those only the ones whose key
// changed are built. A level that would hold more than half its slots — a world whose budget
// deferred deactivations — falls back to the whole rebuild, which withholds its farthest tiles.
// Until 2026-10-03 every rebuild derived every held tile's key and searched every level's chunk
// list twice: 7.5 ms of the 27 a rebuild took at 100 m/s, and 3.8 ms more to put the lists back.
bool TerrainTileSet::rebuild_changes(const TerrainRingLayout& target, f64 time_s,
                                     jobs::JobSystem* jobs, u32& moved, std::string* error) {
  const i64 rebuild_started = time::monotonic_ns();
  moved = 0;
  for (u32 level = 0; level < k_max_terrain_levels; ++level) {
    added_[level].clear();
    released_[level].clear();
  }
  // Every tile whose drawn level may change, with its ring before this rebuild.
  touched_.clear();
  const auto touch = [&](u64 key) {
    if (touched_.find_value(key) != nullptr) return;
    const u8* ring = built_ring_.find_value(key);
    touched_.insert(key, ring != nullptr ? *ring : k_tile_gone);
  };
  for (const TerrainTile& t : taken_) {
    const u64 key = packed(t.x, t.z);
    touch(key);
    if (t.ring == k_tile_gone) {
      built_ring_.erase(key);
    } else {
      built_ring_.insert_or_assign(key, t.ring);
    }
  }
  taken_.clear();
  const auto moved_level = [&](u32 level) {
    return target.cx[level] != layout_.cx[level] || target.cz[level] != layout_.cz[level] ||
           target.half[level] != layout_.half[level];
  };
  // A level's window moving, or the finest far level's square (which lets a ring's tile be drawn
  // or holds it back, `inside_far`), changes a held tile's drawn level only where it changes
  // whether the window covers the tile or the square holds it: those tiles are asked again, and no
  // others. One pass of arithmetic over the held tiles; until 2026-10-03 every held tile of a moved
  // level, and with the finest far square every held tile, was asked again — the 13,000 of the
  // endless desert's rings a few times a kilometre, most of a far square's 2.5 ms scan.
  const bool finest_far_moved = far_ > 0 && moved_level(far_);
  u32 far_moved = 0;  // the far levels whose square moved, for the phases
  for (u32 level = 1; level <= far_; ++level)
    far_moved |= moved_level(level) ? 1u << level : 0u;
  u32 ring_moved = 0;
  for (u32 level = far_ + 1; level < levels_; ++level)
    ring_moved |= moved_level(level) ? 1u << level : 0u;
  // Tiles the last whole rebuild held back for a level's slots (a world whose budget deferred its
  // deactivations) are drawn by neither predicate's change, so while there are any, every held tile
  // of a moved level is asked again, as before.
  if (ring_moved != 0 || finest_far_moved) {
    const bool ask_all = budget_withheld_ > 0;
    for (u32 k = 0; k < built_ring_.size(); ++k) {
      const u32 level = level_of_ring(built_ring_.value_at(k));
      if ((ring_moved & (1u << level)) == 0 && !finest_far_moved) continue;
      const TerrainTile t = tile_of(built_ring_.key_at(k), 0);
      if (ask_all || covers(level, layout_, t.x, t.z) != covers(level, target, t.x, t.z) ||
          inside_far(layout_, t.x, t.z) != inside_far(target, t.x, t.z))
        touch(built_ring_.key_at(k));
    }
    budget_withheld_ = 0;  // asked again; a whole rebuild below counts them afresh
  }
  // Each touched tile's level now; the ones that changed.
  changed_keys_.clear();
  for (u32 k = 0; k < touched_.size(); ++k) {
    const u64 key = touched_.key_at(k);
    const u8 old_ring = touched_.value_at(k);
    const TerrainTile t = tile_of(key, 0);
    const u8* drawn = level_of_.find_value(key);
    const u8 old_level = drawn != nullptr ? *drawn : u8{0};
    if (old_ring != k_tile_gone && old_level == 0) {
      const u32 l = level_of_ring(old_ring);
      if (!covers(l, layout_, t.x, t.z) && window_withheld_[l] > 0) --window_withheld_[l];
    }
    const u8* ring = built_ring_.find_value(key);
    u8 new_level = 0;
    if (ring != nullptr) {
      const u32 l = level_of_ring(*ring);
      if (!covers(l, target, t.x, t.z)) {
        ++window_withheld_[l];
      } else if (inside_far(target, t.x, t.z)) {
        new_level = static_cast<u8>(l);
      }
    }
    if (new_level == old_level) continue;
    if (old_level != 0) {
      level_of_.erase(key);
      --count_[old_level];
    }
    if (new_level != 0) {
      level_of_.insert(key, new_level);
      ++count_[new_level];
    }
    changed_keys_.push_back(key);
  }
  for (u32 level = 1; level < levels_; ++level) {
    if (capacity_[level].slots != 0 && count_[level] > capacity_[level].slots / 2) {
      // Past half a level's slots: the whole rebuild decides which to withhold.
      building_.clear();
      for (u32 k = 0; k < built_ring_.size(); ++k)
        building_.push_back(tile_of(built_ring_.key_at(k), built_ring_.value_at(k)));
      std::sort(building_.begin(), building_.end(), tile_before);
      whole_last_ = true;
      if (!rebuild(target, time_s, jobs, moved, error)) return false;
      index_chunks();
      return true;
    }
  }
  whole_last_ = false;
  // The tiles whose key may have changed: those whose level did, and their neighbours.
  dirty_.clear();
  for (const u64 key : changed_keys_) {
    const TerrainTile t = tile_of(key, 0);
    for (i32 dz = -1; dz <= 1; ++dz) {
      for (i32 dx = -1; dx <= 1; ++dx)
        dirty_.push_back(packed(t.x + dx, t.z + dz));
    }
  }
  std::sort(dirty_.begin(), dirty_.end());
  dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
  struct Want {
    TerrainTileMeshSpec spec;
    u64 key = 0;
  };
  Vector<Want> todo;
  drops_.clear();  // (level << 24 | index) of every chunk let go
  for (const u64 key : dirty_) {
    const TerrainTile t = tile_of(key, 0);
    const u8* level = level_of_.find_value(key);
    const u32* at = chunk_at_.find_value(key);
    if (level == nullptr) {
      if (at != nullptr) drops_.push_back(*at);
      continue;
    }
    Want w;
    w.spec = spec_of(t.x, t.z, *level, level_of_, target);
    w.key = terrain_tile_key(w.spec);
    if (at != nullptr && (*at >> 24) == *level && chunks_[*level][*at & 0xFFFFFFu].key == w.key)
      continue;
    if (at != nullptr) drops_.push_back(*at);
    todo.push_back(w);
  }
  // **The far levels** (renderer.md, "Ground to the horizon"). A level whose square moved, or whose
  // hole did (the next finer far level's square), is laid out again whole — a few hundred tiles,
  // a few times a kilometre — and its tiles kept, built or let go by key. Otherwise only the finest
  // can change, where the rings' tiles did: each far tile holding a world tile whose drawn level
  // changed is asked again, since its hole is the world tiles the rings draw.
  for (u32 level = 1; level <= far_; ++level) {
    const bool whole = moved_level(level) || (level < far_ && moved_level(level + 1));
    const HashMap<u64, u32>& at = far_at_[level];
    if (whole) {
      far_tiles(level, target, level_of_, far_want_);
      far_seen_.clear();
      for (const FarWant& w : far_want_) {
        const u64 key = packed(w.spec.x, w.spec.z);
        far_seen_.insert(key, u8{1});
        const u32* index = at.find_value(key);
        if (index != nullptr && chunks_[level][*index].key == w.key) continue;
        if (index != nullptr) drops_.push_back((level << 24) | *index);
        todo.push_back(Want{w.spec, w.key});
      }
      for (u32 c = 0; c < chunks_[level].size(); ++c) {
        if (far_seen_.find_value(packed(chunks_[level][c].i, chunks_[level][c].j)) == nullptr)
          drops_.push_back((level << 24) | c);
      }
      continue;
    }
    if (level != far_ || changed_keys_.empty()) continue;
    const i32 side = static_cast<i32>(far_tile_mm_[level] / tile_mm_);
    far_keys_.clear();
    for (const u64 key : changed_keys_) {
      const TerrainTile t = tile_of(key, 0);
      far_keys_.push_back(
          packed(static_cast<i32>(floor_div(t.x, side)), static_cast<i32>(floor_div(t.z, side))));
    }
    std::sort(far_keys_.begin(), far_keys_.end());
    far_keys_.erase(std::unique(far_keys_.begin(), far_keys_.end()), far_keys_.end());
    for (const u64 key : far_keys_) {
      const TerrainTile f = tile_of(key, 0);
      FarWant w;
      const bool draws = far_tile(level, target, level_of_, f.x, f.z, w);
      const u32* index = at.find_value(key);
      if (draws && index != nullptr && chunks_[level][*index].key == w.key) continue;
      if (index != nullptr) drops_.push_back((level << 24) | *index);
      if (draws) todo.push_back(Want{w.spec, w.key});
    }
  }
  const i64 scanned = time::monotonic_ns();
  Vector<TerrainChunk> built(todo.size());
  Vector<u8> failed(todo.size(), u8{0});
  std::atomic<i64> heights_ns{0};
  std::atomic<i64> mesh_ns{0};
  const auto run = [&](u32 begin, u32 end) {
    TerrainTileMesh mesh;
    Vector<f32> h;
    for (u32 t = begin; t < end; ++t) {
      if (!build_tile(todo[t].spec, todo[t].key, time_s, built[t], h, mesh, heights_ns, mesh_ns))
        failed[t] = 1;
    }
  };
  if (jobs == nullptr || todo.size() < 2) {
    run(0, todo.size());
  } else {
    jobs->parallel_for(jobs::Pool::Performance, todo.size(), 1,
                       [&](u32 begin, u32 end) { run(begin, end); });
  }
  for (const u8 f : failed) {
    if (f != 0) {
      if (error != nullptr)
        *error = "world tiles: a tile's heights or its cluster LOD could not be made";
      return false;
    }
  }
  const i64 built_at = time::monotonic_ns();
  // Under the lock `padding` takes: the drops (by swap with each level's last chunk, the largest
  // index first so no index still to drop moves), then the new chunks at the ends.
  std::lock_guard<std::mutex> lock(mutex_);
  std::sort(drops_.begin(), drops_.end(), [](u32 a, u32 b) { return a > b; });
  u32 touched_levels = 0;
  for (const u32 d : drops_)
    touched_levels |= 1u << (d >> 24);
  for (const Want& w : todo)
    touched_levels |= 1u << w.spec.level;
  for (u32 level = 1; level < levels_; ++level) {
    if ((touched_levels & (1u << level)) != 0) previous_[level].clear();
  }
  // Where a chunk's place is kept: a ring's tile in `chunk_at_` by its world tile, a far tile in
  // its level's `far_at_` by its own coordinates.
  const auto place = [&](u32 level, i32 i, i32 j, u32 index) {
    if (is_far(level)) {
      far_at_[level].insert_or_assign(packed(i, j), index);
    } else {
      chunk_at_.insert_or_assign(packed(i, j), (level << 24) | index);
    }
  };
  for (const u32 d : drops_) {
    const u32 level = d >> 24;
    const u32 index = d & 0xFFFFFFu;
    Vector<TerrainChunk>& list = chunks_[level];
    TerrainChunk& gone = list[index];
    if (is_far(level)) {
      far_at_[level].erase(packed(gone.i, gone.j));
    } else {
      chunk_at_.erase(packed(gone.i, gone.j));
    }
    if (gone.slot != ~0u) released_[level].push_back(gone.slot);
    previous_[level].push_back(std::move(gone));
    const u32 last = list.size() - 1;
    if (index != last) {
      list[index] = std::move(list[last]);
      place(level, list[index].i, list[index].j, index);
    }
    list.pop_back();
  }
  for (u32 t = 0; t < todo.size(); ++t) {
    const u32 level = todo[t].spec.level;
    Vector<TerrainChunk>& list = chunks_[level];
    const u32 index = list.size();
    place(level, todo[t].spec.x, todo[t].spec.z, index);
    list.push_back(std::move(built[t]));
    added_[level].push_back(index);
  }
  moved = touched_levels & ~1u;
  layout_ = target;
  withheld_ = 0;
  for (u32 level = 1; level < levels_; ++level)
    withheld_ += window_withheld_[level];
  last_built_ = todo.size();
  last_kept_ = 0;
  for (u32 level = 1; level < levels_; ++level)
    last_kept_ += chunks_[level].size();
  last_kept_ -= std::min<u32>(last_kept_, todo.size());
  last_phases_.scan_ms = static_cast<f64>(scanned - rebuild_started) / 1.0e6;
  last_phases_.build_ms = static_cast<f64>(built_at - scanned) / 1.0e6;
  last_phases_.heights_cpu_ms = static_cast<f64>(heights_ns.load()) / 1.0e6;
  last_phases_.mesh_cpu_ms = static_cast<f64>(mesh_ns.load()) / 1.0e6;
  last_phases_.swap_ms = static_cast<f64>(time::monotonic_ns() - built_at) / 1.0e6;
  last_phases_.far_built = 0;
  for (const Want& w : todo)
    last_phases_.far_built += is_far(w.spec.level) ? 1u : 0u;
  last_phases_.far_moved = far_moved;
  ENGINE_LOG_DEBUG(log_renderer, "world tiles rebuilt", log::field("built", last_built_),
                   log::field("kept", last_kept_), log::field("scan_ms", last_phases_.scan_ms),
                   log::field("build_ms", last_phases_.build_ms),
                   log::field("heights_cpu_ms", last_phases_.heights_cpu_ms),
                   log::field("mesh_cpu_ms", last_phases_.mesh_cpu_ms),
                   log::field("swap_ms", last_phases_.swap_ms),
                   log::field("far_built", last_phases_.far_built),
                   log::field("far_moved", last_phases_.far_moved));
  return true;
}

bool TerrainTileSet::build_tile(const TerrainTileMeshSpec& spec, u64 key, f64 time_s,
                                TerrainChunk& chunk, Vector<f32>& h, TerrainTileMesh& mesh,
                                std::atomic<i64>& heights_ns, std::atomic<i64>& mesh_ns) const {
  const u32 a = spec.cells + 3;
  h.assign(static_cast<usize>(a) * a, 0.0f);
  const i32 i0 = spec.x * static_cast<i32>(spec.cells) - 1;
  const i32 j0 = spec.z * static_cast<i32>(spec.cells) - 1;
  const i64 t0 = time::monotonic_ns();
  // The level's own answer: a far level's lattice filtered to its spacing, as its fields are. Its
  // rest heights are what the padding measures the fields against and what its DAG simplifies; from
  // the ground at each point (filter 0, as this asked until 2026-10-03) a far tile's padding was
  // the gap between the filtered and the point-sampled ground (a metre on the test's dunes), and a
  // set built ahead, which keeps the finest lattice only where it was built, answered zeros past it
  // — flat rest meshes, three metres of padding, and a different cut of the rings beside them.
  if (!source_.filtered(time_s, spec.spacing_mm, lattice_[spec.level].filter_mm, i0, j0, a, a,
                        std::span<f32>(h.data(), h.size())))
    return false;
  const i64 t1 = time::monotonic_ns();
  heights_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
  build_terrain_tile_mesh(spec, std::span<const f32>(h.data(), h.size()), mesh);
  chunk.i = spec.x;
  chunk.j = spec.z;
  chunk.corner_x_mm = mesh.corner_x_mm;
  chunk.corner_z_mm = mesh.corner_z_mm;
  chunk.key = key;
  chunk.slot = ~0u;
  chunk.grid_vertices = mesh.positions.size();  // no skirts
  chunk.rest_time_s = time_s;
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(mesh.normals.data(), mesh.normals.size());
  attributes.uvs = std::span<const Vec2>(mesh.uvs.data(), mesh.uvs.size());
  attributes.locked = std::span<const u8>(mesh.locked.data(), mesh.locked.size());
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
          std::span<const u32>(mesh.indices.data(), mesh.indices.size()), options_, chunk.lod,
          nullptr, attributes)) {
    return false;
  }
  // The rest heights: the tile's lattice points at the level's spacing, which every vertex of every
  // level of its DAG is one of (a vertex drawn from a coarser level too: that lattice is part of
  // this one).
  chunk.rest_window = gfx::TerrainField{};
  chunk.rest_window.i0 = i0 + 1;
  chunk.rest_window.j0 = j0 + 1;
  chunk.rest_window.nx = spec.cells + 1;
  chunk.rest_window.nz = spec.cells + 1;
  chunk.rest.resize(static_cast<usize>(spec.cells + 1) * (spec.cells + 1));
  for (u32 j = 0; j <= spec.cells; ++j) {
    for (u32 i = 0; i <= spec.cells; ++i)
      chunk.rest[j * (spec.cells + 1) + i] = h[(j + 1) * a + i + 1];
  }
  mesh_ns.fetch_add(time::monotonic_ns() - t1, std::memory_order_relaxed);
  return true;
}

// The tiles `building_` holds, laid out for `target`: every tile's key from its level and its
// neighbours'; a chunk whose tile and key are unchanged kept with its slot and rest, the rest built
// from the source at `time_s` on `jobs`; the lists swapped under the lock `padding` takes, and what
// they replace kept as `previous_` until the next rebuild.
bool TerrainTileSet::rebuild(const TerrainRingLayout& target, f64 time_s, jobs::JobSystem* jobs,
                             u32& moved, std::string* error) {
  const i64 rebuild_started = time::monotonic_ns();
  moved = 0;
  LevelMap level_of;
  level_of.reserve(building_.size());
  u32 withheld = 0;
  // A tile its level's window does not cover is withheld: nothing could draw it right.
  Vector<u8> tile_level(building_.size(), u8{0});
  for (u32& w : window_withheld_)
    w = 0;
  for (u32 k = 0; k < building_.size(); ++k) {
    const TerrainTile& tile = building_[k];
    const u32 level = level_of_ring(tile.ring);
    if (!covers(level, target, tile.x, tile.z)) {
      ++withheld;
      ++window_withheld_[level];
      continue;
    }
    if (!inside_far(target, tile.x, tile.z)) {
      ++withheld;  // a far level draws it
      continue;
    }
    tile_level[k] = static_cast<u8>(level);
    level_of.insert(packed(tile.x, tile.z), static_cast<u8>(level));
  }
  // Each level's tiles in tile order, with their specs and keys.
  struct Want {
    TerrainTileMeshSpec spec;
    u64 key = 0;
  };
  Vector<Want> want[k_max_terrain_levels];
  for (u32 k = 0; k < building_.size(); ++k) {
    if (tile_level[k] == 0) continue;
    const TerrainTile& tile = building_[k];
    Want w;
    w.spec = spec_of(tile.x, tile.z, tile_level[k], level_of, target);
    w.key = terrain_tile_key(w.spec);
    want[w.spec.level].push_back(w);
  }
  // A level draws at most half its slots' worth of tiles, so a rebuild's new tiles always find
  // free slots beside the ones drawn: past it — a world whose budget deferred the deactivations
  // behind a fast camera — the tiles farthest from the level's window centre are withheld.
  budget_withheld_ = 0;
  for (u32 level = 1; level < levels_; ++level) {
    const u32 most = capacity_[level].slots / 2;
    Vector<Want>& list = want[level];
    if (most == 0 || list.size() <= most) continue;
    Vector<std::pair<i64, u32>> by_distance;
    by_distance.reserve(list.size());
    for (u32 w = 0; w < list.size(); ++w) {
      const i64 dx =
          (static_cast<i64>(list[w].spec.x) * tile_mm_ + tile_mm_ / 2) - target.cx[level];
      const i64 dz =
          (static_cast<i64>(list[w].spec.z) * tile_mm_ + tile_mm_ / 2) - target.cz[level];
      by_distance.push_back({dx * dx + dz * dz, w});
    }
    std::sort(by_distance.begin(), by_distance.end());
    Vector<u8> keep(list.size(), u8{0});
    for (u32 k = 0; k < most; ++k)
      keep[by_distance[k].second] = 1;
    Vector<Want> kept_list;
    kept_list.reserve(most);
    for (u32 w = 0; w < list.size(); ++w) {
      if (keep[w] != 0) {
        kept_list.push_back(list[w]);
      } else {
        ++withheld;
        ++budget_withheld_;
        level_of.erase(packed(list[w].spec.x, list[w].spec.z));  // the far level draws it
      }
    }
    list = std::move(kept_list);
  }
  // The far levels round what the rings draw (renderer.md, "Ground to the horizon").
  for (u32 level = 1; level <= far_; ++level) {
    far_tiles(level, target, level_of, far_want_);
    want[level].reserve(far_want_.size());
    for (const FarWant& w : far_want_)
      want[level].push_back(Want{w.spec, w.key});
  }
  // Keep what is unchanged; build the rest.
  struct Job {
    u32 level = 0;
    u32 index = 0;
  };
  Vector<Job> todo;
  Vector<TerrainChunk> next[k_max_terrain_levels];
  Vector<u8> taken[k_max_terrain_levels];
  u32 kept = 0;
  for (u32 level = 1; level < levels_; ++level) {
    const Vector<TerrainChunk>& old = chunks_[level];
    taken[level].assign(old.size(), u8{0});
    HashMap<u64, u32> old_at;
    old_at.reserve(old.size());
    for (u32 o = 0; o < old.size(); ++o)
      old_at.insert(packed(old[o].i, old[o].j), o);
    next[level].resize(want[level].size());
    for (u32 w = 0; w < want[level].size(); ++w) {
      const Want& wanted = want[level][w];
      const u32* o = old_at.find_value(packed(wanted.spec.x, wanted.spec.z));
      if (o != nullptr && old[*o].key == wanted.key) {
        taken[level][*o] = 1;
        ++kept;
        continue;
      }
      todo.push_back(Job{level, w});
    }
  }
  Vector<u8> failed(todo.size(), u8{0});
  const i64 scanned = time::monotonic_ns();
  std::atomic<i64> heights_ns{0};
  std::atomic<i64> mesh_ns{0};
  const auto run = [&](u32 begin, u32 end) {
    TerrainTileMesh mesh;
    Vector<f32> h;
    for (u32 t = begin; t < end; ++t) {
      const Job& job = todo[t];
      const auto& wanted = want[job.level][job.index];
      if (!build_tile(wanted.spec, wanted.key, time_s, next[job.level][job.index], h, mesh,
                      heights_ns, mesh_ns))
        failed[t] = 1;
    }
  };
  if (jobs == nullptr || todo.size() < 2) {
    run(0, todo.size());
  } else {
    jobs->parallel_for(jobs::Pool::Performance, todo.size(), 1,
                       [&](u32 begin, u32 end) { run(begin, end); });
  }
  const i64 built_at = time::monotonic_ns();
  last_phases_.scan_ms = static_cast<f64>(scanned - rebuild_started) / 1.0e6;
  last_phases_.build_ms = static_cast<f64>(built_at - scanned) / 1.0e6;
  last_phases_.heights_cpu_ms = static_cast<f64>(heights_ns.load()) / 1.0e6;
  last_phases_.mesh_cpu_ms = static_cast<f64>(mesh_ns.load()) / 1.0e6;
  last_phases_.far_built = 0;
  last_phases_.far_moved = 0;
  for (const Job& job : todo)
    last_phases_.far_built += is_far(job.level) ? 1u : 0u;
  for (u32 level = 1; level <= far_; ++level) {
    const bool m = target.cx[level] != layout_.cx[level] || target.cz[level] != layout_.cz[level];
    last_phases_.far_moved |= m ? 1u << level : 0u;
  }
  for (const u8 f : failed) {
    if (f != 0) {
      if (error != nullptr)
        *error = "world tiles: a tile's heights or its cluster LOD could not be made";
      return false;
    }
  }
  // The kept chunks into their places and the lists swapped, all under the lock `padding` takes:
  // the field worker may be measuring a field against these chunks' rests meanwhile.
  std::lock_guard<std::mutex> lock(mutex_);
  for (u32 level = 1; level < levels_; ++level) {
    Vector<TerrainChunk>& old = chunks_[level];
    HashMap<u64, u32> old_at;
    old_at.reserve(old.size());
    for (u32 o = 0; o < old.size(); ++o)
      old_at.insert(packed(old[o].i, old[o].j), o);
    bool changed = want[level].size() != old.size();
    for (u32 w = 0; w < want[level].size(); ++w) {
      const Want& wanted = want[level][w];
      const u32* o = old_at.find_value(packed(wanted.spec.x, wanted.spec.z));
      if (o != nullptr && taken[level][*o] != 0 && old[*o].key == wanted.key) {
        next[level][w] = std::move(old[*o]);
      } else {
        changed = true;
      }
    }
    Vector<TerrainChunk> replaced;
    for (u32 o = 0; o < old.size(); ++o) {
      if (taken[level][o] == 0) replaced.push_back(std::move(old[o]));
    }
    chunks_[level] = std::move(next[level]);
    // A level with nothing changed keeps what it replaced last: that is still what its fields may
    // be drawn over until the swap of the rebuild that replaced it.
    if (!changed && replaced.empty()) continue;
    previous_[level] = std::move(replaced);
    moved |= 1u << level;
  }
  layout_ = target;
  layout_.generation = target.generation;
  withheld_ = withheld;
  last_built_ = todo.size();
  last_kept_ = kept;
  last_phases_.swap_ms = static_cast<f64>(time::monotonic_ns() - built_at) / 1.0e6;
  ENGINE_LOG_DEBUG(log_renderer, "world tiles rebuilt", log::field("built", last_built_),
                   log::field("kept", last_kept_), log::field("scan_ms", last_phases_.scan_ms),
                   log::field("build_ms", last_phases_.build_ms),
                   log::field("heights_cpu_ms", last_phases_.heights_cpu_ms),
                   log::field("mesh_cpu_ms", last_phases_.mesh_cpu_ms),
                   log::field("swap_ms", last_phases_.swap_ms));
  return true;
}

}  // namespace engine::renderer
