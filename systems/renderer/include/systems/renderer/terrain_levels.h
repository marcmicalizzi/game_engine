#pragma once

// **Terrain levels of chunks, and who lays them out** (docs/subsystems/renderer.md, "The rings in
// the scene", "The ground from the world's tiles"; ADR-0050). A moving terrain is drawn as terrain
// levels: level 0 is the scene's own grid, and every other level is a set of **chunks** — cluster
// meshes on the world's lattice at the level's spacing, each in a slot of the GPU scene's, moved by
// the pool's terrain stage from the level's two fields at the one surface time every level shares.
// That drawing half is one mechanism (`GpuScene`'s slots and arenas, `TerrainMotion`'s re-centre,
// freeze, carry-over and swap). What decides which chunks a level has is not, and this interface is
// the line between the two:
//
//   - `TerrainRingSet` (terrain_rings.h) lays chunks out as squares round the camera, by the ground
//     provider's own ring rules, and leaves the scene's grid to draw round them;
//   - `TerrainTileSet` (terrain_tiles.h) lays them out as the world's tiles, one chunk a tile at
//   the
//     level its world ring gives it, the set decided by the world's ring, and leaves the scene's
//     grid nothing to draw.
//
// A **layout** is where each level stands — the square a ring's chunks cover, or the window a tile
// level's fields cover — and, for the tiles, which set of tiles it is (`generation`). `GpuScene`
// reserves each level's slots from `capacity`; `TerrainMotion` asks `next_layout` every frame, and
// when it differs has the chunks it changes rebuilt off the frame (`prepare` on the frame's thread,
// then `update` on its worker), uploaded, and swapped in at once.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/scene_gen/tile_source.h>
#include <systems/renderer/terrain.h>

#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// The most levels a terrain has: the scene's grid and up to seven more (a ring set has at most
// three rings, `terrain::k_max_rings` counting the scene's grid; a tile set one level per world
// ring).
inline constexpr u32 k_max_terrain_levels = 8;

// One chunk of one level, as the renderer keeps it: where it is, what decides its mesh, its slot
// in the GPU scene once it has one, its DAG until it is uploaded, and its **rest heights** — the
// heights its DAG was built from, what its bounds and LOD errors were fit to and what a moving
// field's padding is measured against.
struct TerrainChunk {
  i32 i = 0;
  i32 j = 0;
  u64 key = 0;
  u32 slot = ~0u;
  u32 grid_vertices = 0;         // a DAG vertex whose source is at or past this is a skirt's
  geometry::ClusterLodMesh lod;  // emptied once the GPU scene has it
  f64 rest_time_s = 0.0;
  // The rest heights at the chunk's lattice points, rows of x in order of z over `rest_window` (NaN
  // where the chunk has no vertex: its share of a ring's hole).
  gfx::TerrainField rest_window;
  Vector<f32> rest;
};

// Where the levels stand (level 0 unused): a centre and a half-side in millimetres per level — a
// ring's square, or a tile level's field window — and, for a tile set, which set of tiles
// (`generation`, 0 for rings).
struct TerrainRingLayout {
  i64 cx[k_max_terrain_levels] = {};
  i64 cz[k_max_terrain_levels] = {};
  i64 half[k_max_terrain_levels] = {};
  u64 generation = 0;
  bool operator==(const TerrainRingLayout&) const = default;
};

class TerrainLevelSet {
 public:
  virtual ~TerrainLevelSet();

  virtual bool valid() const noexcept = 0;
  // Levels including the scene's grid.
  virtual u32 level_count() const noexcept = 0;
  virtual const TerrainDesc& desc() const noexcept = 0;
  virtual const TerrainLattice& lattice(u32 level) const noexcept = 0;
  // Metres a level's skirts hang below its border (0: it has none).
  virtual f32 skirt_m(u32 level) const noexcept = 0;
  // Whether the scene's grid (level 0) is drawn, and what it leaves out under a layout: round the
  // rings, the middle ring's square; under tiles, the whole world, and nothing is drawn of it.
  virtual bool grid_drawn() const noexcept = 0;
  virtual Vec4 grid_hole(const TerrainRingLayout& layout) const noexcept = 0;
  // Whether every level other than the grid draws some of its vertices from another level's
  // description (a tile's border along a coarser tile), so a level's cull padding must cover what
  // those levels' fields do: the tiles'.
  virtual bool shares_vertices() const noexcept { return false; }
  // The source a level's heights come from, or null for the scene's own ground (the rings).
  virtual const scene_gen::TileSource* source() const noexcept { return nullptr; }

  // The layout as built last. What the frame draws may be an older one, which its holder keeps.
  virtual TerrainRingLayout layout() const noexcept = 0;
  // The lattice window a level's fields cover under a layout, apron included.
  virtual gfx::TerrainField field_window(u32 level,
                                         const TerrainRingLayout& layout) const noexcept = 0;
  // The most samples `field_window(level, ...)` has for any layout: what a field slot holds.
  virtual u64 field_capacity(u32 level) const noexcept = 0;
  // Where the levels are to stand for a camera at (x, z), from `from`: `from` itself when nothing
  // changes. Called by the frame, which may ask while a rebuild runs on the worker.
  virtual TerrainRingLayout next_layout(f32 camera_x, f32 camera_z,
                                        const TerrainRingLayout& from) const noexcept = 0;

  // What the GPU scene reserves for a level: slots, clusters a slot holds, and the vertex and
  // triangle arenas.
  struct Capacity {
    u32 slots = 0;
    u32 clusters_per_slot = 0;
    u64 vertices = 0;
    u64 triangles = 0;
  };
  virtual Capacity capacity(u32 level) const noexcept = 0;
  // Every chunk of a level as built last.
  virtual Vector<TerrainChunk>& chunks(u32 level) noexcept = 0;

  // **A rebuild to `target`**, one at a time. `prepare` runs on the frame's thread when the frame
  // asks for it, before anything else touches the set for it; `update` then runs on the worker,
  // with nothing touching the chunk lists but `padding`: the chunks whose key changed are rebuilt
  // from the heights at `time_s` — read from `fields[level]` where given and covering, asked for
  // otherwise — every other chunk kept with its slot, its rest and no DAG. `moved` is a mask of the
  // levels whose chunk lists changed; `chunks(level)` is then the new list, new chunks with no
  // slot.
  struct Heights {
    const f32* heights = nullptr;
    gfx::TerrainField window;
  };
  virtual void prepare(const TerrainRingLayout& target) { (void)target; }
  virtual bool update(f32 camera_x, f32 camera_z, f64 time_s, const TerrainRingLayout& target,
                      std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
                      std::string* error) = 0;

  // **The padding a level's spheres take while `field` is drawn**: the largest |field - rest| over
  // the level's chunks as built last *and* as they were before the last `update` that moved them.
  virtual f64 padding(u32 level, std::span<const f32> field,
                      const gfx::TerrainField& window) const = 0;
  static f64 padding_of(std::span<const TerrainChunk> chunks, std::span<const f32> field,
                        const gfx::TerrainField& window);

  // Milliseconds the last `build` or `update` took, and how many chunks it built and kept.
  virtual f64 last_build_ms() const noexcept = 0;
  virtual u32 last_built() const noexcept = 0;
  virtual u32 last_kept() const noexcept = 0;
};

}  // namespace engine::renderer
