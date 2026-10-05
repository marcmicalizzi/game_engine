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
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/scene_gen/tile_source.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_levels.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// The most world rings a tile set draws: a level each, beside the scene's grid.
inline constexpr u32 k_max_tile_rings = 7;
// The most far levels past them (renderer.md, "Ground to the horizon"), so that the grid, the
// rings and the far levels fit `k_max_terrain_levels`.
inline constexpr u32 k_max_far_levels = k_max_terrain_levels - 1 - k_max_tile_rings;

// A change's ring for a tile the world let go (`TerrainTileSet::change_tiles`).
inline constexpr u8 k_tile_gone = 0xFF;

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
  // **The far levels** (renderer.md, "Ground to the horizon"): the renderer's own ground past the
  // outermost ring, out to where the air takes it — `far_levels` square rings of larger tiles round
  // the camera, each twice the spacing and twice the reach of the one inside it, the first
  // `far_ratio` times the outermost ring's spacing, every tile `far_cells` cells a side, their
  // heights filtered to their spacing (`scene_gen::Lattice::filter_mm`). None by default here; a
  // host takes the count from `RenderSettings::terrain_far_levels`.
  u32 far_levels = 0;
  u32 far_cells = 16;
  u32 far_ratio = 4;
};

// A tile's cells a side in `ring`: the description's, or 64 in the inner ring halving outwards to
// 8 (50 cm, 1 m, 2 m and then 4 m on 32 m tiles).
u32 terrain_tile_cells(const TerrainTilesDesc& desc, u32 ring) noexcept;
// A scene's tiles from its `world` block (its rings and each ring's `ground_cells`; the world's
// default rings, 1.5, 8 and 24 tiles of 32 m, where it names none or has no block), and
// `far_levels` far levels past them (negative: `renderer.terrain.far_levels`).
struct WorldDesc;
TerrainTilesDesc terrain_tiles_desc(const WorldDesc& world, i32 far_levels = 0) noexcept;
// The far levels a host draws when its settings say -1: `renderer.terrain.far_levels`.
u32 terrain_far_levels_default() noexcept;
// False, with a sentence naming what is wrong: a tile that is not a whole number of millimetres, no
// ring or more than `k_max_tile_rings`, radii that are not positive and increasing, cells that do
// not cut the tile into whole millimetres, a ring finer than the one inside it, or two neighbouring
// rings whose cells do not divide (a coarser tile's lattice must be a subset of a finer one's);
// more than `k_max_far_levels`, a far ratio under two, or far cells that do not make a far tile a
// whole number of world tiles cut on the first far level's lattice.
bool validate_terrain_tiles(const TerrainTilesDesc& desc, std::string* error = nullptr);

// **Where the far levels stand** (renderer.md, "Ground to the horizon"), millimetres, for a
// description: far level k (1 the finest, past the outermost ring) has its lattice `spacing`
// apart, tiles `tile` wide, a square window `half` either side of a centre on a multiple of
// `snap` (the next coarser level's tile), which moves when the camera is more than `margin` from
// it. Each level's square holds the one inside it whatever the camera does (the first holds every
// tile the world's rings can hold), so a level is its square less the square inside it — the first
// less the world's tiles — and every level's border is on the next one's lattice.
struct TerrainFarLevel {
  i64 spacing = 0;
  i64 tile = 0;
  i64 half = 0;
  i64 snap = 0;
  i64 margin = 0;
};
// The far level `k` (1..desc.far_levels) of a valid description.
TerrainFarLevel terrain_far_level(const TerrainTilesDesc& desc, u32 k) noexcept;
// How far the ground reaches from a camera at the worst place its windows let it stand, metres: the
// last far level's half-side less how far its centre can be from the camera (the outermost ring's
// reach with no far level).
f64 terrain_far_reach_m(const TerrainTilesDesc& desc) noexcept;

// **The ring's first-fill rule**: every tile whose centre is within the outermost radius of the
// camera on the ground, in the first ring whose radius its centre's distance is within, in tile
// order (x, then z) — what the world's ring holds after its first update from nothing with no
// budget. Each tile's centre is measured from the camera in f64, so the rule is the same 10,000 km
// out as by the origin: a tile set moved by whole tiles with its camera holds the same tiles.
void terrain_tiles_round(const TerrainTilesDesc& desc, WorldPos camera, Vector<TerrainTile>& out);
// The same rule in the world ring's own float32 arithmetic (absolute float32 metres, as
// `TierAssignment::tier_of` scores a tile), which its tests compare the world's first update with.
// ADR-0053 seam: systems/world's ring scores from a float32 observer; this goes when it takes
// WorldPos. By the origin the two differ only on a centre exactly on a ring's radius.
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
  // The tile's corner, its lattice point (0, 0), in whole millimetres from the world's origin; the
  // positions are metres from it (renderer.md, "The ground's tiles are placed at their corners").
  i64 corner_x_mm = 0;
  i64 corner_z_mm = 0;
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
  i32 x = 0;  // in tiles of this level's own size (a far level's are larger than the world's)
  i32 z = 0;
  u8 level = 1;
  u32 cells = 32;
  i64 spacing_mm = 1000;
  TerrainTileNeighbours neighbours;
  // Cells a side of each level (index = level), for the ratio a coarser edge is collapsed by, when
  // every level's tile is the same size; or each level's spacing, which says it whatever their
  // tiles are (a far level's), and wins where it is set.
  u32 level_cells[k_max_terrain_levels] = {};
  i64 level_spacing_mm[k_max_terrain_levels] = {};
  // **A far tile's hole** (the first far level's, over the world's tiles a ring draws): bit
  // `b * hole_side + a` set leaves out the square of `hole_cells` cells a side at (a, b), whose
  // border is then locked and drawn from this level — the finer tile beside it collapses onto it.
  u64 hole_mask = 0;
  u32 hole_side = 0;
  u32 hole_cells = 0;
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

  // Lays the first layout out round the camera at `camera` (its x and z) by the ring's first-fill
  // rule and builds every tile of it from `source` at the terrain's own time, on `jobs` when given,
  // and sizes each level's slots and arenas. False, with a sentence, for a description
  // `validate_terrain_tiles` refuses, a terrain grid whose half-side is not whole millimetres (its
  // UV frame is the tiles'), or a source with no heights. `terrain` and whatever `source` points at
  // must outlive the set.
  bool build(const TerrainDesc& terrain, const TerrainTilesDesc& tiles,
             const scene_gen::TileSource& source, WorldPos camera, jobs::JobSystem* jobs,
             std::string* error = nullptr);

  // **The tiles the world holds**, and their rings, as its ring hands them over after an update:
  // what the next rebuild draws. Any order; a tile named twice keeps its first ring. True when it
  // differs from the set handed over before (and the next frame asks for a rebuild). Linear in the
  // set: what differs becomes changes, as `change_tiles` takes them.
  bool set_tiles(std::span<const TerrainTile> tiles);
  // **What changed in the tiles the world holds** since the last hand-over: a tile now held in
  // `ring` (it entered, or changed ring), or let go (`ring == k_tile_gone`), in the order it
  // happened. Costs what changed — the world's drawn-ground consumer hands its events through it
  // (world.md, "The consumers") — and so does the rebuild it asks for. True when anything did.
  bool change_tiles(std::span<const TerrainTile> changes);
  // How many hand-overs changed the set.
  u64 generation() const noexcept { return generation_; }
  // The tiles the world holds as handed over last, in no particular order.
  u32 held_count() const noexcept { return held_.size(); }
  TerrainTile held_at(u32 i) const noexcept {
    const u64 key = held_.key_at(i);
    return TerrainTile{static_cast<i32>(static_cast<u32>(key >> 32)),
                       static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFFu)), held_.value_at(i)};
  }
  const TerrainTilesDesc& tiles_desc() const noexcept { return tiles_; }
  // The level a world ring is drawn at, and back. Levels count coarsest first: the grid is 0, the
  // far levels 1..`far_count()` (the coarsest first), then the rings, the outermost first.
  u32 level_of_ring(u32 ring) const noexcept {
    return ring < tiles_.ring_count ? far_ + tiles_.ring_count - ring : far_ + 1u;
  }
  u32 ring_of_level(u32 level) const noexcept { return far_ + tiles_.ring_count - level; }
  u32 cells(u32 level) const noexcept { return level_cells_[level]; }
  // **The far levels** (renderer.md, "Ground to the horizon"): how many, whether a level is one,
  // and which of them (1 the finest) it is.
  u32 far_count() const noexcept { return far_; }
  bool is_far(u32 level) const noexcept { return level >= 1 && level <= far_; }
  u32 far_index(u32 level) const noexcept { return far_ + 1 - level; }
  // A far level's tile size, mm, and the far tiles it draws: their coordinates in its own tiles and
  // their hole masks (the finest far level's; 0 elsewhere).
  i64 far_tile_mm(u32 level) const noexcept { return far_tile_mm_[level]; }
  // The far level whose square holds world tile (x, z) under `layout`, the finest that does; 0 for
  // none.
  u8 far_level_at(const TerrainRingLayout& layout, i32 x, i32 z) const noexcept;
  // The spec a drawn chunk's mesh is a function of under the layout built last, derived again
  // from what the set holds: what its key must be, and what a test builds its mesh from. False for
  // no such chunk.
  bool chunk_spec(u32 level, u32 index, TerrainTileMeshSpec& out) const;

  // Where each drawn tile's chunk is: its level, and its place in `chunks(level)`. False for a tile
  // the set does not draw (not held, or withheld: its level's window does not cover it).
  bool find(i32 x, i32 z, u32& level, u32& index) const noexcept;
  // Tiles the last rebuild left out because their level's window does not cover them.
  u32 withheld() const noexcept { return withheld_; }
  // Where the last rebuild's time went: deciding what to build (the keys, on the worker's thread),
  // building it (wall, on the pool) and the CPU milliseconds its jobs spent on the source's heights
  // and on the meshes and DAGs, and putting the lists in place under the lock. And what of it was
  // the far levels': the far tiles built, and the far levels whose square moved (bit k for level
  // k).
  struct Phases {
    f64 scan_ms = 0.0;
    f64 build_ms = 0.0;
    f64 heights_cpu_ms = 0.0;
    f64 mesh_cpu_ms = 0.0;
    f64 swap_ms = 0.0;
    u32 far_built = 0;
    u32 far_moved = 0;
  };
  const Phases& last_phases() const noexcept { return last_phases_; }

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
  u32 far_levels() const noexcept override { return far_; }
  TerrainRingLayout layout() const noexcept override;
  gfx::TerrainField field_window(u32 level,
                                 const TerrainRingLayout& layout) const noexcept override;
  u64 field_capacity(u32 level) const noexcept override;
  TerrainRingLayout next_layout(WorldPos camera,
                                const TerrainRingLayout& from) const noexcept override;
  Capacity capacity(u32 level) const noexcept override { return capacity_[level]; }
  Vector<TerrainChunk>& chunks(u32 level) noexcept override { return chunks_[level]; }
  const Vector<TerrainChunk>& chunks(u32 level) const noexcept { return chunks_[level]; }
  void prepare(const TerrainRingLayout& target) override;
  bool update(WorldPos camera, f64 time_s, const TerrainRingLayout& target,
              std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
              std::string* error) override;
  // ADR-0053 seam: systems/world's tests hand the camera as float32 metres by the origin; they take
  // WorldPos when the world's agent moves them. Exact widenings of the three above.
  bool build(const TerrainDesc& terrain, const TerrainTilesDesc& tiles,
             const scene_gen::TileSource& source, f32 camera_x, f32 camera_z, jobs::JobSystem* jobs,
             std::string* error = nullptr) {
    return build(terrain, tiles, source, widened(camera_x, camera_z), jobs, error);
  }
  TerrainRingLayout next_layout(f32 camera_x, f32 camera_z,
                                const TerrainRingLayout& from) const noexcept {
    return next_layout(widened(camera_x, camera_z), from);
  }
  bool update(f32 camera_x, f32 camera_z, f64 time_s, const TerrainRingLayout& target,
              std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
              std::string* error) {
    return update(widened(camera_x, camera_z), time_s, target, fields, jobs, moved, error);
  }
  static WorldPos widened(f32 x, f32 z) noexcept {
    return WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)};
  }
  f64 padding(u32 level, std::span<const f32> field,
              const gfx::TerrainField& window) const override;
  bool changed_only() const noexcept override { return !whole_last_; }
  std::span<const u32> added(u32 level) const noexcept override;
  std::span<const u32> released(u32 level) const noexcept override;
  f64 padding_added(u32 level, std::span<const f32> field,
                    const gfx::TerrainField& window) const override;
  f64 last_build_ms() const noexcept override { return last_build_ms_; }
  u32 last_built() const noexcept override { return last_built_; }
  u32 last_kept() const noexcept override { return last_kept_; }

  // The window's centre a camera at `camera` (its x and z) puts a level's fields at, millimetres:
  // its tile's corner.
  void window_centre(u32 level, WorldPos camera, i64& cx, i64& cz) const noexcept;

 private:
  // Whether `layout`'s window of `level` covers tile (x, z), apron and all.
  bool covers(u32 level, const TerrainRingLayout& layout, i32 x, i32 z) const noexcept;
  // With far levels, whether the finest far level's square holds world tile (x, z) a tile in, so
  // every tile round it is drawn: a ring's tile is drawn only where both hold.
  bool inside_far(const TerrainRingLayout& layout, i32 x, i32 z) const noexcept;
  // A far level's tiles under `layout`: the spec and key of every tile it draws, given what the
  // rings draw (`level_of`); and one of them, false when the level draws nothing there (outside its
  // square, inside the next finer one's, or — the finest — wholly under the rings' tiles).
  struct FarWant {
    TerrainTileMeshSpec spec;
    u64 key = 0;
  };
  void far_tiles(u32 level, const TerrainRingLayout& layout, const HashMap<u64, u8>& level_of,
                 Vector<FarWant>& out) const;
  bool far_tile(u32 level, const TerrainRingLayout& layout, const HashMap<u64, u8>& level_of, i32 x,
                i32 z, FarWant& out) const;
  // The whole rebuild (the first fill, and a level past half its slots), and the one that costs
  // what changed (every other).
  bool rebuild(const TerrainRingLayout& target, f64 time_s, jobs::JobSystem* jobs, u32& moved,
               std::string* error);
  bool rebuild_changes(const TerrainRingLayout& target, f64 time_s, jobs::JobSystem* jobs,
                       u32& moved, std::string* error);
  void index_chunks();
  TerrainTileMeshSpec spec_of(i32 x, i32 z, u8 level, const HashMap<u64, u8>& level_of,
                              const TerrainRingLayout& layout) const noexcept;
  bool build_tile(const TerrainTileMeshSpec& spec, u64 key, f64 time_s, TerrainChunk& chunk,
                  Vector<f32>& h, TerrainTileMesh& mesh, std::atomic<i64>& heights_ns,
                  std::atomic<i64>& mesh_ns) const;

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
  i64 half_mm_[k_max_terrain_levels] = {};      // a level's window: half its side
  i64 margin_mm_[k_max_terrain_levels] = {};    // how far the camera goes before it moves
  u32 far_ = 0;                                 // far levels, 1..far_
  i64 far_tile_mm_[k_max_terrain_levels] = {};  // a far level's tile
  i64 far_snap_mm_[k_max_terrain_levels] = {};  // where its window's centre may stand
  // Each far level's drawn tiles, by their coordinates in its tiles, to their place in its list;
  // and the worker's scratch for laying them out.
  HashMap<u64, u32> far_at_[k_max_terrain_levels];
  Vector<FarWant> far_want_;
  HashMap<u64, u8> far_seen_;
  Vector<u64> far_keys_;
  Capacity capacity_[k_max_terrain_levels];
  Vector<TerrainChunk> chunks_[k_max_terrain_levels];
  Vector<TerrainChunk> previous_[k_max_terrain_levels];
  TerrainRingLayout layout_;
  // The frame's: the tiles the world holds (tile → ring) and what changed since the last rebuild
  // was asked for, which `prepare` hands to the worker (`taken_`).
  HashMap<u64, u8> held_;
  HashMap<u64, u8> incoming_;  // set_tiles' scratch
  Vector<TerrainTile> changes_;
  Vector<TerrainTile> taken_;
  // The worker's: the held tiles as of the last rebuild, every drawn tile's level and place in
  // its level's list (level << 24 | index), each level's count and the tiles its window leaves
  // out; and its scratch. `building_` is the whole rebuild's list.
  HashMap<u64, u8> built_ring_;
  HashMap<u64, u8> level_of_;
  HashMap<u64, u32> chunk_at_;
  u32 count_[k_max_terrain_levels] = {};
  u32 window_withheld_[k_max_terrain_levels] = {};
  HashMap<u64, u8> touched_;
  Vector<u64> changed_keys_;
  Vector<u64> dirty_;
  Vector<u32> drops_;
  // What the last rebuild changed (`added`, `released`), unless it was a whole one.
  Vector<u32> added_[k_max_terrain_levels];
  Vector<u32> released_[k_max_terrain_levels];
  bool whole_last_ = true;
  // Held tiles the last whole rebuild held back for a level's slots, not yet asked again.
  u32 budget_withheld_ = 0;
  Vector<TerrainTile> building_;
  u64 generation_ = 0;
  u32 withheld_ = 0;
  f64 last_build_ms_ = 0.0;
  u32 last_built_ = 0;
  u32 last_kept_ = 0;
  Phases last_phases_;
};

}  // namespace engine::renderer
