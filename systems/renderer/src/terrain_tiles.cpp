// The ground from the world's tiles (terrain_tiles.h; docs/subsystems/renderer.md, "The ground from
// the world's tiles"; ADR-0049).
#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain_tiles.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// Read when the tiles are built (renderer.md, "The ground from the world's tiles").
tunables::Float tile_slack{"renderer.terrain.tile_slack", 1.5, 1.0, 8.0,
                           "How much room each world tile's GPU slot and its level's arenas keep "
                           "above the largest tile the first layout built, as a factor"};

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

// The world's tile order: x, then z, signed (world.md, "Why this shape").
bool tile_before(const TerrainTile& a, const TerrainTile& b) noexcept {
  return a.x < b.x || (a.x == b.x && a.z < b.z);
}

u64 packed(i32 x, i32 z) noexcept {
  return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(z));
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

TerrainTilesDesc terrain_tiles_desc(const WorldDesc& world) noexcept {
  TerrainTilesDesc t;
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
  return true;
}

void terrain_tiles_round(const TerrainTilesDesc& desc, f32 x, f32 z, Vector<TerrainTile>& out) {
  out.clear();
  if (desc.ring_count == 0) return;
  const f32 t = desc.tile_size;
  const f32 reach = desc.radius[desc.ring_count - 1] * t;
  const i32 x0 = static_cast<i32>(std::floor(static_cast<f64>(x - reach) / t)) - 1;
  const i32 x1 = static_cast<i32>(std::floor(static_cast<f64>(x + reach) / t)) + 1;
  const i32 z0 = static_cast<i32>(std::floor(static_cast<f64>(z - reach) / t)) - 1;
  const i32 z1 = static_cast<i32>(std::floor(static_cast<f64>(z + reach) / t)) + 1;
  const Vec3 eye{x, 0.0f, z};
  // The world ring's arithmetic, as it scores a tile: its centre on the ground, the distance in
  // floats, and the first ring whose boundary it is strictly inside (`TierAssignment::tier_of`).
  for (i32 i = x0; i <= x1; ++i) {
    for (i32 j = z0; j <= z1; ++j) {
      const Vec3 centre{(static_cast<f32>(i) + 0.5f) * t, 0.0f, (static_cast<f32>(j) + 0.5f) * t};
      const f32 d = distance(centre, eye);
      for (u32 r = 0; r < desc.ring_count; ++r) {
        if (d < desc.radius[r] * t) {
          out.push_back(TerrainTile{i, j, static_cast<u8>(r)});
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
    const u32 nc = spec.level_cells[nl];
    if (nc == 0 || c % nc != 0 || nc >= c) continue;
    edge_level[e] = nl;
    ratio[e] = c / nc;
  }
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
      const u32 va = rep(i, j);
      const u32 vb = rep(i + 1, j);
      const u32 vc = rep(i, j + 1);
      const u32 vd = rep(i + 1, j + 1);
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
      out.positions.push_back(Vec3{static_cast<f32>(static_cast<f64>(xm) / 1000.0), y,
                                   static_cast<f32>(static_cast<f64>(zm) / 1000.0)});
      out.uvs.push_back(Vec2{static_cast<f32>(static_cast<f64>(xm - spec.uv_x0_mm) * uv_scale),
                             static_cast<f32>(static_cast<f64>(zm - spec.uv_z0_mm) * uv_scale)});
      const bool border = i == 0 || j == 0 || i == c || j == c;
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

void TerrainTileSet::window_centre(u32 level, f32 camera_x, f32 camera_z, i64& cx,
                                   i64& cz) const noexcept {
  (void)level;
  const f64 t = static_cast<f64>(tile_mm_) / 1000.0;
  cx = static_cast<i64>(std::floor(static_cast<f64>(camera_x) / t)) * tile_mm_;
  cz = static_cast<i64>(std::floor(static_cast<f64>(camera_z) / t)) * tile_mm_;
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

TerrainRingLayout TerrainTileSet::next_layout(f32 camera_x, f32 camera_z,
                                              const TerrainRingLayout& from) const noexcept {
  TerrainRingLayout out = from;
  if (!valid()) return out;
  out.generation = generation_;
  for (u32 level = 1; level < levels_; ++level) {
    i64 cx = 0;
    i64 cz = 0;
    window_centre(level, camera_x, camera_z, cx, cz);
    // The camera has gone more than the margin from the window's centre, or a tile the world holds
    // at this level is outside it: the window moves to the camera.
    bool move =
        std::max(std::abs(cx - from.cx[level]), std::abs(cz - from.cz[level])) > margin_mm_[level];
    if (!move) {
      for (const TerrainTile& t : wanted_) {
        if (level_of_ring(t.ring) == level && !covers(level, from, t.x, t.z)) {
          move = true;
          break;
        }
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
  incoming_.assign(tiles.begin(), tiles.end());
  // Sorted in place: a stable sort takes a buffer from the heap every frame, and the world never
  // hands the same tile twice.
  std::sort(incoming_.begin(), incoming_.end(), tile_before);
  incoming_.erase(std::unique(incoming_.begin(), incoming_.end(),
                              [](const TerrainTile& a, const TerrainTile& b) {
                                return a.x == b.x && a.z == b.z;
                              }),
                  incoming_.end());
  if (incoming_ == wanted_) return false;
  std::swap(incoming_, wanted_);
  ++generation_;
  return true;
}

void TerrainTileSet::prepare(const TerrainRingLayout& target) {
  (void)target;
  building_.assign(wanted_.begin(), wanted_.end());
}

bool TerrainTileSet::find(i32 x, i32 z, u32& level, u32& index) const noexcept {
  for (u32 l = 1; l < levels_; ++l) {
    for (u32 c = 0; c < chunks_[l].size(); ++c) {
      if (chunks_[l][c].i == x && chunks_[l][c].j == z) {
        level = l;
        index = c;
        return true;
      }
    }
  }
  return false;
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
                           const scene_gen::TileSource& source, f32 camera_x, f32 camera_z,
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
  uv_x0_mm_ = -extent;
  uv_size_mm_ = 2 * extent;
  levels_ = tiles.ring_count + 1;
  lattice_[0] = terrain_scene_lattice(terrain);
  level_cells_[0] = 0;
  const f64 t = static_cast<f64>(tiles.tile_size);
  for (u32 level = 1; level < levels_; ++level) {
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
    window_centre(level, camera_x, camera_z, layout_.cx[level], layout_.cz[level]);
    layout_.half[level] = half_mm_[level];
  }
  Vector<TerrainTile> first;
  terrain_tiles_round(tiles, camera_x, camera_z, first);
  wanted_.clear();
  generation_ = 0;
  set_tiles(std::span<const TerrainTile>(first.data(), first.size()));
  layout_.generation = generation_;
  building_.assign(wanted_.begin(), wanted_.end());
  u32 moved = 0;
  if (!rebuild(layout_, terrain.time_s, jobs, moved, error)) {
    levels_ = 0;
    return false;
  }
  for (u32 level = 1; level < levels_; ++level)
    previous_[level].clear();
  // What the GPU scene reserves a level: two slots for every tile its ring can hold (one drawn and
  // one replacing it), counted with the camera anywhere in its tile, each slot and the arenas room
  // for the largest tile built here with `renderer.terrain.tile_slack` on top.
  const f64 slack = tile_slack.get();
  for (u32 level = 1; level < levels_; ++level) {
    const u32 ring = ring_of_level(level);
    const f64 reach =
        static_cast<f64>(tiles.radius[ring]) * (1.0 + static_cast<f64>(tiles.hysteresis)) + 0.7072;
    const i32 span = static_cast<i32>(std::ceil(reach)) + 1;
    u64 most_tiles = 0;
    for (i32 i = -span; i <= span; ++i) {
      for (i32 j = -span; j <= span; ++j) {
        const f64 d = std::hypot(static_cast<f64>(i), static_cast<f64>(j));
        if (d <= reach) ++most_tiles;
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
      spec.uv_x0_mm = uv_x0_mm_;
      spec.uv_z0_mm = uv_x0_mm_;
      spec.uv_size_mm = uv_size_mm_;
      const u32 a = spec.cells + 3;
      Vector<f32> h(static_cast<usize>(a) * a);
      (void)source_.heights(terrain.time_s, spec.spacing_mm, -1, -1, a, a,
                            std::span<f32>(h.data(), h.size()));
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
    ENGINE_LOG_INFO(log_renderer, "world tile level", log::field("level", level),
                    log::field("ring", ring), log::field("cells", level_cells_[level]),
                    log::field("spacing_m", lattice_[level].spacing),
                    log::field("tiles", chunks_[level].size()), log::field("slots", c.slots),
                    log::field("clusters_per_slot", c.clusters_per_slot),
                    log::field("vertices", c.vertices), log::field("triangles", c.triangles),
                    log::field("window_m", 2.0 * static_cast<f64>(half_mm_[level]) / 1000.0));
  }
  last_build_ms_ = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  return true;
}

bool TerrainTileSet::update(f32 camera_x, f32 camera_z, f64 time_s, const TerrainRingLayout& target,
                            std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
                            std::string* error) {
  (void)camera_x;
  (void)camera_z;
  (void)fields;
  moved = 0;
  if (!valid()) return true;
  const i64 started = time::monotonic_ns();
  if (!rebuild(target, time_s, jobs, moved, error)) return false;
  last_build_ms_ = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  return true;
}

// The tiles `building_` holds, laid out for `target`: every tile's key from its level and its
// neighbours'; a chunk whose tile and key are unchanged kept with its slot and rest, the rest built
// from the source at `time_s` on `jobs`; the lists swapped under the lock `padding` takes, and what
// they replace kept as `previous_` until the next rebuild.
bool TerrainTileSet::rebuild(const TerrainRingLayout& target, f64 time_s, jobs::JobSystem* jobs,
                             u32& moved, std::string* error) {
  moved = 0;
  LevelMap level_of;
  level_of.reserve(building_.size());
  u32 withheld = 0;
  // A tile its level's window does not cover is withheld: nothing could draw it right.
  Vector<u8> tile_level(building_.size(), u8{0});
  for (u32 k = 0; k < building_.size(); ++k) {
    const TerrainTile& tile = building_[k];
    const u32 level = level_of_ring(tile.ring);
    if (!covers(level, target, tile.x, tile.z)) {
      ++withheld;
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
    TerrainTileMeshSpec& spec = w.spec;
    spec.x = tile.x;
    spec.z = tile.z;
    spec.level = tile_level[k];
    spec.cells = level_cells_[spec.level];
    spec.spacing_mm = lattice_[spec.level].spacing_mm;
    for (u32 l = 0; l < levels_; ++l)
      spec.level_cells[l] = level_cells_[l];
    spec.uv_x0_mm = uv_x0_mm_;
    spec.uv_z0_mm = uv_x0_mm_;
    spec.uv_size_mm = uv_size_mm_;
    const i32 x = tile.x;
    const i32 z = tile.z;
    spec.neighbours.edge[0] = level_at(level_of, x - 1, z);
    spec.neighbours.edge[1] = level_at(level_of, x + 1, z);
    spec.neighbours.edge[2] = level_at(level_of, x, z - 1);
    spec.neighbours.edge[3] = level_at(level_of, x, z + 1);
    const i32 dx[4] = {-1, 1, -1, 1};
    const i32 dz[4] = {-1, -1, 1, 1};
    for (u32 c = 0; c < 4; ++c) {
      spec.neighbours.corner[c] =
          coarsest(coarsest(level_at(level_of, x + dx[c], z), level_at(level_of, x, z + dz[c])),
                   level_at(level_of, x + dx[c], z + dz[c]));
    }
    w.key = terrain_tile_key(spec);
    want[spec.level].push_back(w);
  }
  // A level draws at most half its slots' worth of tiles, so a rebuild's new tiles always find
  // free slots beside the ones drawn: past it — a world whose budget deferred the deactivations
  // behind a fast camera — the tiles farthest from the level's window centre are withheld.
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
      }
    }
    list = std::move(kept_list);
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
  const auto run = [&](u32 begin, u32 end) {
    TerrainTileMesh mesh;
    Vector<f32> h;
    for (u32 t = begin; t < end; ++t) {
      const Job& job = todo[t];
      const TerrainTileMeshSpec& spec = want[job.level][job.index].spec;
      const u32 a = spec.cells + 3;
      h.assign(static_cast<usize>(a) * a, 0.0f);
      const i32 i0 = spec.x * static_cast<i32>(spec.cells) - 1;
      const i32 j0 = spec.z * static_cast<i32>(spec.cells) - 1;
      if (!source_.heights(time_s, spec.spacing_mm, i0, j0, a, a,
                           std::span<f32>(h.data(), h.size()))) {
        failed[t] = 1;
        continue;
      }
      build_terrain_tile_mesh(spec, std::span<const f32>(h.data(), h.size()), mesh);
      TerrainChunk& chunk = next[job.level][job.index];
      chunk.i = spec.x;
      chunk.j = spec.z;
      chunk.key = want[job.level][job.index].key;
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
        failed[t] = 1;
        continue;
      }
      // The rest heights: the tile's lattice points at the level's spacing, which every vertex of
      // every level of its DAG is one of (a vertex drawn from a coarser level too: that lattice is
      // part of this one).
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
  return true;
}

}  // namespace engine::renderer
