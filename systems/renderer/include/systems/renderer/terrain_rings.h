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
// **slots** — a mesh and an instance each, placed at its chunk's corner when it is turned on (the
// provider builds a chunk in its corner's frame, `scene_gen::RingChunkRef`), with a fixed run of
// clusters and pairs — and
// arenas of vertices and triangles, all sized from the rings built at load (`capacity`). A chunk
// drawn is a slot whose mesh names the chunk's clusters; a re-centre builds the chunks whose cells,
// hole or border changed (`terrain::TerrainRings::update`), uploads them into free slots over as
// many frames as it takes, and then, in one frame, turns the new slots on and the replaced ones off
// — the per-block path of a streamed world's tiles (renderer.md, "Instances that come and go")
// applied to meshes rather than instances: a change costs the chunks it changed, never the ring.
// So there are twice as many slots as a ring has chunks, and twice its vertices and triangles.
//
// **The rings are the ground's.** The ring tunables, the layout and re-centre rules and the chunks'
// meshes and DAGs come from the ground provider the terrain names (`scene_gen::GroundRings`, which
// the terrain capability's dunes make; the waves have none), found through the scene-generator
// registry, so the renderer links nothing of the capability that builds them.
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
#include <systems/renderer/terrain_levels.h>

#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// A ring's `TerrainRingLayout` entry is its square: centre and half-side in millimetres.
class TerrainRingSet final : public TerrainLevelSet {
 public:
  TerrainRingSet();
  ~TerrainRingSet() override;
  TerrainRingSet(const TerrainRingSet&) = delete;
  TerrainRingSet& operator=(const TerrainRingSet&) = delete;

  // Lays the rings out round the camera at (x, z), metres, with the terrain capability's ring
  // tunables (`terrain.rings.*`), and builds every moving ring's chunks from the field at the
  // terrain's own time, on `jobs` when given. The scene's grid is the outer ring and is never built
  // here. False, with a sentence, for a terrain whose ground provider makes no rings (the waves; a
  // provider this build does not carry), a scene grid whose spacing is not a whole number of
  // millimetres, or tunables that leave no moving ring. `desc` must outlive the set.
  bool build(const TerrainDesc& desc, WorldPos camera, jobs::JobSystem* jobs,
             std::string* error = nullptr);
  bool valid() const noexcept override { return levels_ > 1; }
  // Levels including the scene's grid: 1 + the moving rings.
  u32 level_count() const noexcept override { return levels_; }
  // The layout's ring index of level k > 0 (the terrain capability counts rings innermost first).
  u32 ring_of_level(u32 level) const noexcept { return levels_ - 1 - level; }

  const TerrainDesc& desc() const noexcept override { return *desc_; }
  const TerrainLattice& lattice(u32 level) const noexcept override { return lattice_[level]; }
  f32 skirt_m(u32 level) const noexcept override { return skirt_m_[level]; }
  // The scene's grid is the outer ring, drawn round the middle ring's square.
  bool grid_drawn() const noexcept override { return true; }
  Vec4 grid_hole(const TerrainRingLayout& layout) const noexcept override {
    return square(1, layout);
  }

  // The layout as built last. What the frame draws may be an older one, which its holder keeps.
  TerrainRingLayout layout() const noexcept override;
  // A ring level's square in metres (x0, z0, x1, z1): what the level round it leaves out.
  Vec4 square(u32 level, const TerrainRingLayout& layout) const noexcept;
  // The lattice window a level's fields cover for a layout: the ring's square, rounded out to the
  // lattice, and a sample of apron either side for the normals. The scene grid's is the whole
  // grid. A re-centre evaluates its moved levels' fields over the new layout's windows, so nothing
  // is evaluated ahead for where a ring may go next.
  gfx::TerrainField field_window(u32 level,
                                 const TerrainRingLayout& layout) const noexcept override;
  // The most samples `field_window(level, ...)` has for any layout: what a field slot holds.
  u64 field_capacity(u32 level) const noexcept override;
  // Whether `window` covers level's ring under `layout`, apron and all: what a field must do to be
  // drawn under that layout.
  bool covers(u32 level, const TerrainRingLayout& layout,
              const gfx::TerrainField& window) const noexcept;
  // Where the terrain capability's re-centre rule (`terrain::recentre_rings`) puts the rings for a
  // camera at (x, z) when they stand at `from`: `from` itself when nothing moves. It reads only the
  // ring parameters, so the frame may ask while the worker runs an `update`.
  TerrainRingLayout next_layout(WorldPos camera,
                                const TerrainRingLayout& from) const noexcept override;
  // Whether `next_layout` moves anything.
  bool wants_update(WorldPos camera, const TerrainRingLayout& layout) const noexcept;

  // What the GPU scene reserves for a ring level: room for two rings the size of the largest this
  // layout rule can make, from the chunks built at load, with `renderer.terrain.ring_slack` on top.
  Capacity capacity(u32 level) const noexcept override { return capacity_[level]; }

  // Every chunk of a ring level as built last, in chunk order.
  Vector<TerrainChunk>& chunks(u32 level) noexcept override { return chunks_[level]; }
  const Vector<TerrainChunk>& chunks(u32 level) const noexcept { return chunks_[level]; }

  // **A re-centre** (`TerrainLevelSet::update`): the rule for a camera now at (x, z), metres; the
  // rings it moves are rebuilt from the field at `time_s` — read from `fields[level]` where it
  // covers a chunk, evaluated otherwise. The rule decides where the rings go; `target`, which the
  // frame worked out with the same rule, is the motion's to check against `layout()` after.
  bool update(WorldPos camera, f64 time_s, std::span<const Heights> fields, jobs::JobSystem* jobs,
              u32& moved, std::string* error = nullptr);
  bool update(WorldPos camera, f64 time_s, const TerrainRingLayout& target,
              std::span<const Heights> fields, jobs::JobSystem* jobs, u32& moved,
              std::string* error) override {
    (void)target;
    return update(camera, time_s, fields, jobs, moved, error);
  }

  // **The padding a level's spheres take while `field` is drawn** (`TerrainLevelSet::padding`): a
  // field is drawn over the chunks built last until the frame that swaps them for the ones before,
  // and over the others after. `field` covers `window` of the level's lattice; rest heights outside
  // it are not measured (a ring's field window covers its chunks, apron and all).
  f64 padding(u32 level, std::span<const f32> field,
              const gfx::TerrainField& window) const override;

  // Milliseconds the last `build` or `update` took, and how many chunks it built and kept.
  f64 last_build_ms() const noexcept override { return last_build_ms_; }
  u32 last_built() const noexcept override { return last_built_; }
  u32 last_kept() const noexcept override { return last_kept_; }

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
