#pragma once

// The terrain rings in the scene (docs/subsystems/renderer.md, "The dunes in time-lapse", "The
// rings in the scene"; docs/subsystems/terrain.md, "Rings"): the ground near the camera at finer
// grids than the scene's, as square rings of chunked cluster meshes the terrain capability builds
// (`terrain::TerrainRings`), drawn as **terrain levels** beside the scene's own grid.
//
// **Levels.** Level 0 is the scene's grid, the cached mesh everything was drawn with before; level
// k > 0 is a moving ring, coarsest first — with the default tunables level 1 is the middle ring (a
// metre, a kilometre either side) and level 2 the inner one (50 cm, 250 m either side). Every
// level is the same function of the same game time at its own spacing, moved by the same pool
// stage, and each level leaves out the square of the level inside it: a ring by its geometry
// (the terrain capability builds each ring with a hole and hangs skirts from both edges), the
// scene's grid, which is one cached mesh and cannot be rebuilt round a hole that moves, by the
// pool pass moving its vertices inside the square onto the square's edge and the cull pass
// dropping its clusters wholly inside it (`gfx::TerrainLevelDesc::hole`).
//
// **A ring is chunks, and a chunk is a slot.** The GPU scene reserves, per ring, a fixed set of
// **slots** — a mesh and an identity instance each, with a fixed run of clusters and pairs — and
// arenas of vertices and triangles, all sized from the rings built at load (`capacity`). A chunk
// drawn is a slot whose mesh names the chunk's clusters; a re-centre builds the chunks whose cells,
// hole or border changed (`terrain::TerrainRings::update`), uploads them into free slots over as
// many frames as it takes, and then, in one frame, turns the new slots on and the replaced ones off
// — the per-block path of a streamed world's tiles (renderer.md, "Instances that come and go")
// applied to meshes rather than instances: a change costs the chunks it changed, never the ring.
// So there are twice as many slots as a ring has chunks, and twice its vertices and triangles.
//
// **What this file owns** is the CPU half: the layout, the chunks' DAGs until they are uploaded,
// each drawn chunk's slot and its **rest heights** (what its bounds and LOD errors were fit to,
// which a moving field's padding is measured from), the lattice and the field window of each
// level, and the heights a chunk is built from — the renderer's own function, the dunes and the
// ridges and basins it adds, at a game time, read from the level's newest field where it covers
// the chunk.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/terrain.h>

#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// The most levels a terrain has: the scene's grid and up to three rings (`terrain::k_max_rings`
// counts the scene's grid as its outer ring).
inline constexpr u32 k_max_terrain_levels = 4;

// One chunk of one ring, as the renderer keeps it: where it is, what decides its mesh, its slot
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
  // where the chunk has no vertex: its share of the ring's hole). 66 KB a chunk.
  gfx::TerrainField rest_window;
  Vector<f32> rest;
};

// Where the rings are, per level (0 unused): centre and half-side in millimetres.
struct TerrainRingLayout {
  i64 cx[k_max_terrain_levels] = {};
  i64 cz[k_max_terrain_levels] = {};
  i64 half[k_max_terrain_levels] = {};
  bool operator==(const TerrainRingLayout&) const = default;
};

class TerrainRingSet {
 public:
  TerrainRingSet();
  ~TerrainRingSet();
  TerrainRingSet(const TerrainRingSet&) = delete;
  TerrainRingSet& operator=(const TerrainRingSet&) = delete;

  // Lays the rings out round the camera at (x, z), metres, with the terrain capability's ring
  // tunables (`terrain.rings.*`), and builds every moving ring's chunks from the field at the
  // terrain's own time, on `jobs` when given. The scene's grid is the outer ring and is never built
  // here. False, with a sentence, for a terrain that is not the dune generator's, a build without
  // the terrain capability, a scene grid whose spacing is not a whole number of millimetres, or
  // tunables that leave no moving ring. `desc` must outlive the set.
  bool build(const TerrainDesc& desc, f32 camera_x, f32 camera_z, jobs::JobSystem* jobs,
             std::string* error = nullptr);
  bool valid() const noexcept { return levels_ > 1; }
  // Levels including the scene's grid: 1 + the moving rings.
  u32 level_count() const noexcept { return levels_; }
  // The layout's ring index of level k > 0 (the terrain capability counts rings innermost first).
  u32 ring_of_level(u32 level) const noexcept { return levels_ - 1 - level; }

  const TerrainDesc& desc() const noexcept { return *desc_; }
  const TerrainLattice& lattice(u32 level) const noexcept { return lattice_[level]; }
  f32 skirt_m(u32 level) const noexcept { return skirt_m_[level]; }

  // The layout as built last. What the frame draws may be an older one, which its holder keeps.
  TerrainRingLayout layout() const noexcept;
  // A ring level's square in metres (x0, z0, x1, z1): what the level round it leaves out.
  Vec4 square(u32 level, const TerrainRingLayout& layout) const noexcept;
  // The lattice window a level's fields cover for a layout: the ring's square, rounded out to the
  // lattice, and a sample of apron either side for the normals. The scene grid's is the whole
  // grid. A re-centre evaluates its moved levels' fields over the new layout's windows, so nothing
  // is evaluated ahead for where a ring may go next.
  gfx::TerrainField field_window(u32 level, const TerrainRingLayout& layout) const noexcept;
  // The most samples `field_window(level, ...)` has for any layout: what a field slot holds.
  u64 field_capacity(u32 level) const noexcept;
  // Whether `window` covers level's ring under `layout`, apron and all: what a field must do to be
  // drawn under that layout.
  bool covers(u32 level, const TerrainRingLayout& layout,
              const gfx::TerrainField& window) const noexcept;
  // Where the terrain capability's re-centre rule (`terrain::recentre_rings`) puts the rings for a
  // camera at (x, z) when they stand at `from`: `from` itself when nothing moves. It reads only the
  // ring parameters, so the frame may ask while the worker runs an `update`.
  TerrainRingLayout next_layout(f32 camera_x, f32 camera_z,
                                const TerrainRingLayout& from) const noexcept;
  // Whether `next_layout` moves anything.
  bool wants_update(f32 camera_x, f32 camera_z, const TerrainRingLayout& layout) const noexcept;

  // What the GPU scene reserves for a ring level: slots, clusters a slot holds, and the vertex and
  // triangle arenas — room for two rings the size of the largest this layout rule can make, from
  // the chunks built at load, with `renderer.terrain.ring_slack` on top.
  struct Capacity {
    u32 slots = 0;
    u32 clusters_per_slot = 0;
    u64 vertices = 0;
    u64 triangles = 0;
  };
  Capacity capacity(u32 level) const noexcept { return capacity_[level]; }

  // Every chunk of a ring level as built last, in chunk order.
  Vector<TerrainChunk>& chunks(u32 level) noexcept { return chunks_[level]; }
  const Vector<TerrainChunk>& chunks(u32 level) const noexcept { return chunks_[level]; }

  // **A re-centre**, one at a time and with nothing else touching the chunk lists but `padding`
  // (which may run on another thread meanwhile): the rule for a camera now at (x, z), metres; the
  // rings it moves are rebuilt from the field at `time_s` — read from `fields[level]` where it
  // covers a chunk, evaluated otherwise — keeping every chunk whose key is unchanged, with its
  // slot, its rest and no DAG. `moved` is a mask of the levels whose chunk lists changed;
  // `chunks(level)` is then the new list, new chunks with no slot.
  struct Heights {
    const f32* heights = nullptr;
    gfx::TerrainField window;
  };
  bool update(f32 camera_x, f32 camera_z, f64 time_s, std::span<const Heights> fields,
              jobs::JobSystem* jobs, u32& moved, std::string* error = nullptr);

  // **The padding a level's spheres take while `field` is drawn**: the largest |field - rest| over
  // the level's chunks as built last *and* as they were before the last `update` that moved them —
  // a field is drawn over the one set until the frame that swaps it for the other, and over the
  // other after. `field` covers `window` of the level's lattice; rest heights outside it are not
  // measured (a ring's field window covers its chunks, apron and all).
  f64 padding(u32 level, std::span<const f32> field, const gfx::TerrainField& window) const;
  static f64 padding_of(std::span<const TerrainChunk> chunks, std::span<const f32> field,
                        const gfx::TerrainField& window);

  // Milliseconds the last `build` or `update` took, and how many chunks it built and kept.
  f64 last_build_ms() const noexcept { return last_build_ms_; }
  u32 last_built() const noexcept { return last_built_; }
  u32 last_kept() const noexcept { return last_kept_; }

 private:
  struct State;
  void take_chunks(u32 level_mask);
  // `padding` may be asked from another thread while `update` runs: it takes this, and `update`
  // takes it only to swap the chunk lists.
  mutable std::mutex mutex_;
  const TerrainDesc* desc_ = nullptr;
  std::unique_ptr<State> state_;
  u32 levels_ = 0;
  TerrainLattice lattice_[k_max_terrain_levels];
  f32 skirt_m_[k_max_terrain_levels] = {};
  i64 half_mm_[k_max_terrain_levels] = {};
  Capacity capacity_[k_max_terrain_levels];
  Vector<TerrainChunk> chunks_[k_max_terrain_levels];
  // Before the last update that moved a level: the chunks it replaced, rest heights only.
  Vector<TerrainChunk> previous_[k_max_terrain_levels];
  f64 last_build_ms_ = 0.0;
  u32 last_built_ = 0;
  u32 last_kept_ = 0;
};

}  // namespace engine::renderer
