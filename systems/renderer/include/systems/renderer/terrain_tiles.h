#pragma once

// **The ground from the world's tiles** (docs/subsystems/renderer.md, "The ground from the world's
// tiles"; ADR-0050): the terrain drawn as the world's tile grid, one chunk a tile, each tile a
// chunk of the terrain level its world ring gives it — the rings' GPU half (terrain_levels.h) laid
// out by the world's ring instead of by squares round the camera.
//
// **Levels.** Level 0 is the scene's grid, which draws nothing here: its hole covers the world, and
// it stays for the terrain's material, the UV frame its maps are baked over, and as the reference a
// tile is held to. Level k > 0 is world ring `ring_count - k` — level 1 the outermost ring, the
// coarsest — at the tile's edge over the ring's cells a side, on the world's lattice counted from
// its origin in millimetres. A level's tiles are the ones the world's ring holds in its ring: a
// band, and no two levels hold one tile.
//
// **A tile's mesh** is its lattice points at the level's spacing, two counter-clockwise triangles a
// cell with the scene grid's diagonal, its border locked, its UVs in the grid's frame, and a
// cluster DAG built from the heights at the surface's time. **Seams** (ADR-0050 decision 3): where
// a tile meets a coarser one its edge keeps only the coarser lattice's points — every other edge
// vertex is collapsed onto the nearer kept one (the lower on a tie) and the triangles that
// degenerates are dropped — and every vertex it shares with a coarser tile names that tile's level
// in its rest normal (`gfx::terrain_level_normal`), which the pool's terrain stage draws it from; a
// corner names the coarsest of the four tiles round it. So a tile's mesh is a function of its
// **key**: its coordinates, its level, and the levels of the eight tiles round it.
//
// **Fields.** A level's fields cover a square window of its lattice round a centre on the tile
// grid: every tile the level can hold while the camera is within `margin` of the centre, and a
// point of apron. The window moves when the camera leaves the margin, or when a tile of the level
// the world holds is outside it (a first fill far from the last, a deactivation the budget
// deferred); a tile its level's window does not cover is withheld — not drawn — until it does.
//
// **Heights** come from a tile source (scene_gen/tile_source.h) and nothing else: the scene's
// ground provider seen as tiles, or a tile set built ahead; the set does not know which.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/scene_gen/tile_source.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_levels.h>

#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// The most world rings a tile set draws: a level each, beside the scene's grid.
inline constexpr u32 k_max_tile_rings = k_max_terrain_levels - 1;

// One tile the world holds: tile (x, z) covers [x, x + 1) × [z, z + 1) tiles of the world's grid,
// and `ring` is its world ring, innermost 0.
struct TerrainTile {
  i32 x = 0;
  i32 z = 0;
  u8 ring = 0;
  bool operator==(const TerrainTile&) const = default;
};

// A scene's tiles: the world's grid and rings (`engine.scene.WorldRings`), and the ground's cells a
// side of a tile in each ring.
struct TerrainTilesDesc {
  f32 tile_size = 32.0f;  // metres, a whole number of millimetres
  u32 ring_count = 3;
  f32 radius[k_max_tile_rings] = {1.5f, 8.0f, 24.0f};  // tiles, innermost first
  u32 cells[k_max_tile_rings] = {};                    // 0: `terrain_tile_cells`'s rule
  f32 hysteresis = 0.15f;
};

// A tile's cells a side in `ring`: the description's, or 64 in the inner ring halving outwards to
// 8 (50 cm, 1 m, 2 m and then 4 m on 32 m tiles).
u32 terrain_tile_cells(const TerrainTilesDesc& desc, u32 ring) noexcept;
// A scene's tiles from its `world` block (its rings and each ring's `ground_cells`; the world's
// default rings, 1.5, 8 and 24 tiles of 32 m, where it names none or has no block).
struct WorldDesc;
TerrainTilesDesc terrain_tiles_desc(const WorldDesc& world) noexcept;
// False, with a sentence naming what is wrong: a tile that is not a whole number of millimetres, no
// ring or more than `k_max_tile_rings`, radii that are not positive and increasing, cells that do
// not cut the tile into whole millimetres, a ring finer than the one inside it, or two neighbouring
// rings whose cells do not divide (a coarser tile's lattice must be a subset of a finer one's).
bool validate_terrain_tiles(const TerrainTilesDesc& desc, std::string* error = nullptr);

// **The ring's first-fill rule**: every tile whose centre is within the outermost radius of (x, z)
// on the ground, in the first ring whose radius its centre's distance is within, in tile order (x,
// then z) — what the world's ring holds after its first update from nothing with no budget.
void terrain_tiles_round(const TerrainTilesDesc& desc, f32 x, f32 z, Vector<TerrainTile>& out);

// **A tile's neighbourhood**: the levels of the tiles round it that decide its mesh (0: not held).
// Edges are -x, +x, -z, +z; corners (-x, -z), (+x, -z), (-x, +z), (+x, +z).
struct TerrainTileNeighbours {
  u8 edge[4] = {};
  u8 corner[4] = {};  // the coarsest of the three other tiles at the corner
};
// A tile's mesh at `level` (`cells` a side, `spacing_mm` apart) with its neighbours: positions on
// the world's lattice from `heights` — `(cells + 3)^2` of them, a point of apron round the tile,
// rows of x in order of z — rest normals by central differences over them, UVs in the frame
// `uv_x0_mm, uv_z0_mm, uv_size_mm`, the border locked, the edges along coarser tiles collapsed onto
// their lattice, and the vertices another level draws naming it in their normals.
struct TerrainTileMesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u8> locked;
  Vector<u32> indices;
  // Of each vertex: its lattice point, and the level it is drawn from (its own, or a coarser one).
  Vector<i32> lattice_i;
  Vector<i32> lattice_j;
  Vector<u8> drawn_from;
};
struct TerrainTileMeshSpec {
  i32 x = 0;
  i32 z = 0;
  u8 level = 1;
  u32 cells = 32;
  i64 spacing_mm = 1000;
  TerrainTileNeighbours neighbours;
  // Cells a side of each level (index = level), for the ratio a coarser edge is collapsed by.
  u32 level_cells[k_max_terrain_levels] = {};
  i64 uv_x0_mm = 0;
  i64 uv_z0_mm = 0;
  i64 uv_size_mm = 1;
};
void build_terrain_tile_mesh(const TerrainTileMeshSpec& spec, std::span<const f32> heights,
                             TerrainTileMesh& out);
// The key a tile's mesh is a function of, besides its heights.
u64 terrain_tile_key(const TerrainTileMeshSpec& spec) noexcept;

class TerrainTileSet final : public TerrainLevelSet {
 public:
  TerrainTileSet();
  ~TerrainTileSet() override;
  TerrainTileSet(const TerrainTileSet&) = delete;
  TerrainTileSet& operator=(const TerrainTileSet&) = delete;

  // Lays the first layout out round the camera at (x, z) by the ring's first-fill rule and builds
  // every tile of it from `source` at the terrain's own time, on `jobs` when given, and sizes each
  // level's slots and arenas. False, with a sentence, for a description `validate_terrain_tiles`
  // refuses, a terrain grid whose half-side is not whole millimetres (its UV frame is the tiles'),
  // or a source with no heights. `terrain` and whatever `source` points at must outlive the set.
  bool build(const TerrainDesc& terrain, const TerrainTilesDesc& tiles,
             const scene_gen::TileSource& source, f32 camera_x, f32 camera_z, jobs::JobSystem* jobs,
             std::string* error = nullptr);

  // **The tiles the world holds**, and their rings, as its ring hands them over after an update:
  // what the next rebuild draws. Any order; a tile named twice keeps its first ring. True when it
  // differs from the set handed over last (and the next frame asks for a rebuild). Allocates only
  // when the set grows past every set before it.
  bool set_tiles(std::span<const TerrainTile> tiles);
  // The set handed over last, in tile order, and how many sets have been.
  std::span<const TerrainTile> wanted() const noexcept {
    return std::span<const TerrainTile>(wanted_.data(), wanted_.size());
  }
  u64 generation() const noexcept { return generation_; }
  const TerrainTilesDesc& tiles_desc() const noexcept { return tiles_; }
  // The level a world ring is drawn at, and back.
  u32 level_of_ring(u32 ring) const noexcept {
    return ring < tiles_.ring_count ? tiles_.ring_count - ring : 1u;
  }
  u32 ring_of_level(u32 level) const noexcept { return tiles_.ring_count - level; }
  u32 cells(u32 level) const noexcept { return level_cells_[level]; }

  // Where each drawn tile's chunk is: its level, and its place in `chunks(level)`. False for a tile
  // the set does not draw (not held, or withheld: its level's window does not cover it).
  bool find(i32 x, i32 z, u32& level, u32& index) const noexcept;
  // Tiles the last rebuild left out because their level's window does not cover them.
  u32 withheld() const noexcept { return withheld_; }

  // ---- TerrainLevelSet ----
  bool valid() const noexcept override { return levels_ > 1; }
  u32 level_count() const noexcept override { return levels_; }
  const TerrainDesc& desc() const noexcept override { return *desc_; }
  const TerrainLattice& lattice(u32 level) const noexcept override { return lattice_[level]; }
  f32 skirt_m(u32) const noexcept override { return 0.0f; }
  bool grid_drawn() const noexcept override { return false; }
  Vec4 grid_hole(const TerrainRingLayout& layout) const noexcept override;
  bool shares_vertices() const noexcept override { return true; }
  const scene_gen::TileSource* source() const noexcept override { return &source_; }
  TerrainRingLayout layout() const noexcept override;
  gfx::TerrainField field_window(u32 level,
                                 const TerrainRingLayout& layout) const noexcept override;
  u64 field_capacity(u32 level) const noexcept override;
  TerrainRingLayout next_layout(f32 camera_x, f32 camera_z,
                                const TerrainRingLayout& from) const noexcept override;
  Capacity capacity(u32 level) const noexcept override { return capacity_[level]; }
  Vector<TerrainChunk>& chunks(u32 level) noexcept override { return chunks_[level]; }
  const Vector<TerrainChunk>& chunks(u32 level) const noexcept { return chunks_[level]; }
  void prepare(const TerrainRingLayout& target) override;
  bool update(f32 camera_x, f32 camera_z, f64 time_s, const TerrainRingLayout& target,
              std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
              std::string* error) override;
  f64 padding(u32 level, std::span<const f32> field,
              const gfx::TerrainField& window) const override;
  f64 last_build_ms() const noexcept override { return last_build_ms_; }
  u32 last_built() const noexcept override { return last_built_; }
  u32 last_kept() const noexcept override { return last_kept_; }

  // The window's centre a camera at (x, z) puts a level's fields at, millimetres: its tile's
  // corner.
  void window_centre(u32 level, f32 camera_x, f32 camera_z, i64& cx, i64& cz) const noexcept;

 private:
  // Whether `layout`'s window of `level` covers tile (x, z), apron and all.
  bool covers(u32 level, const TerrainRingLayout& layout, i32 x, i32 z) const noexcept;
  bool rebuild(const TerrainRingLayout& target, f64 time_s, jobs::JobSystem* jobs, u32& moved,
               std::string* error);

  // `padding` may be asked from another thread while `update` runs: it takes this, and `update`
  // takes it only to swap the chunk lists.
  mutable std::mutex mutex_;
  const TerrainDesc* desc_ = nullptr;
  TerrainTilesDesc tiles_;
  scene_gen::TileSource source_;
  geometry::ClusterLodOptions options_;
  u32 levels_ = 0;
  i64 tile_mm_ = 32000;
  i64 uv_x0_mm_ = 0;
  i64 uv_size_mm_ = 1;
  TerrainLattice lattice_[k_max_terrain_levels];
  u32 level_cells_[k_max_terrain_levels] = {};
  i64 half_mm_[k_max_terrain_levels] = {};    // a level's window: half its side
  i64 margin_mm_[k_max_terrain_levels] = {};  // how far the camera goes before it moves
  Capacity capacity_[k_max_terrain_levels];
  Vector<TerrainChunk> chunks_[k_max_terrain_levels];
  Vector<TerrainChunk> previous_[k_max_terrain_levels];
  TerrainRingLayout layout_;
  // The tiles the world holds: handed over by the frame (`wanted_`), and taken for a rebuild
  // (`building_`) on the frame's thread before the worker reads it.
  Vector<TerrainTile> wanted_;
  Vector<TerrainTile> incoming_;
  Vector<TerrainTile> building_;
  u64 generation_ = 0;
  u32 withheld_ = 0;
  f64 last_build_ms_ = 0.0;
  u32 last_built_ = 0;
  u32 last_kept_ = 0;
};

}  // namespace engine::renderer
