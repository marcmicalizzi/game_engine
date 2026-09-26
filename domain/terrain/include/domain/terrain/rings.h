#pragma once

// Terrain rings (docs/subsystems/terrain.md, "Rings"): the ground near the camera at a finer grid
// than the scene's, as square rings centred on the camera, each its own mesh with its own cluster
// LOD DAG built by geometry's builder.
//
// **Why rings.** The scene's terrain is one grid (4,097 vertices a side at most, 1.5 m a sample
// over the erg's 6.1 km), and a slip face is five samples wide at that spacing: a faceted plane
// with hard edges near the camera ([third session], "The crest is five samples wide" in Not yet).
// The cluster LOD DAG can only coarsen what it is given, never refine it, so the near ground needs
// a finer source grid, and it only needs one near the camera: an inner ring a few hundred metres
// wide at 50 cm, a middle ring two kilometres wide at a metre, and the scene's own grid for the
// rest, with holes where the finer rings are.
//
// **Every vertex stays where it is in the world.** A ring's corner and its hole's edges are
// multiples of both its own spacing and the next ring's, in millimetres from the world's origin
// (`ring_snap_mm`), so a ring that moves samples the same world points it did (nothing swims),
// its hole's edges are lines of the next ring's grid (no sliver of ground belongs to neither), and
// the outer ring is the scene's grid exactly.
//
// **The re-centre rule.** A ring moves when the camera is more than half its half-side from its
// centre (Chebyshev distance), to the camera snapped to its step, and is clamped inside the ring
// round it; a ring whose hole moved is rebuilt too. A ring k + 1 at least three times ring k's
// half-side keeps ring k inside it however the camera moves (`validate_rings`), so the clamp only
// bites at the scene's edge. Between moves nothing is built.
//
// **Seams are hidden by skirts, not stitched.** Where two rings meet, the finer one has vertices
// between the coarser one's, and each ring's DAG simplifies its own border, so the two edges do not
// meet exactly. Each ring hangs a vertical skirt `skirt_mm` deep from every border edge, facing
// away from its own surface, with the top vertex's normal and UV: any crack shows a skirt the
// colour of the ground beside it.
//
// **The same bytes on any number of threads.** A chunk's heights, mesh and DAG are built on one
// thread, whichever it is, and the chunks are merged in chunk order.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/terrain/dunes.h>

#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::terrain {

// Rings, innermost first; the last is the fixed outer ring (the scene's grid).
inline constexpr u32 k_max_rings = 4;
// The most vertices a side a ring may have: the scene's own grid's ceiling.
inline constexpr i64 k_ring_max_vertices = 8'193;

struct RingSpec {
  i64 half_mm = 0;     // half the square's side
  i64 spacing_mm = 0;  // the grid's
  i64 skirt_mm = 0;    // how far the skirts hang below the border
};

struct RingParams {
  u32 count = 0;  // rings in `ring`, innermost first, the last the outer
  RingSpec ring[k_max_rings];
  // The outer ring's centre, which never moves.
  i64 outer_cx_mm = 0, outer_cz_mm = 0;
  // The UV frame over the whole terrain: u = (x - uv_x0) / uv_size, v likewise in z — the scene
  // grid's, so a ring's UVs are the ones the material maps were baked over.
  i64 uv_x0_mm = 0, uv_z0_mm = 0, uv_size_mm = 1;
};

// The default rings for a terrain whose own grid is `outer_half_mm` either side of `(cx, cz)` at
// `outer_spacing_mm`: an inner ring 250 m either side at 50 cm and a middle ring 1,000 m either
// side at 1 m (the tunables `terrain.rings.*`, read here), the outer the scene's grid, skirts eight
// spacings deep. A ring that does not fit inside the one round it is left out.
RingParams ring_params_from_tunables(i64 cx_mm, i64 cz_mm, i64 outer_half_mm, i64 outer_spacing_mm);

// The step ring k's centre and half-side are multiples of: the least common multiple of its
// spacing and the next ring's (its own spacing for the outer ring).
i64 ring_snap_mm(const RingParams& params, u32 k) noexcept;

// False, with a sentence, for rings the rules above cannot hold for.
bool validate_rings(const RingParams& params, std::string* error = nullptr);

struct Ring {
  i64 cx = 0, cz = 0;  // centre, mm
  i64 half = 0;
  i64 spacing = 0;
  i64 skirt = 0;
  bool has_hole = false;  // every ring but the innermost
  i64 hole_cx = 0, hole_cz = 0, hole_half = 0;
};

struct RingLayout {
  u32 count = 0;
  Ring ring[k_max_rings];
};

// The rings round a camera at (x, z), mm, from scratch.
void place_rings(const RingParams& params, i64 camera_x, i64 camera_z, RingLayout& out);

// The re-centre rule for a camera now at (x, z): moves the rings it says to and returns a mask of
// the rings to rebuild (bit k for ring k) — a ring that moved and the ring whose hole it is. Zero
// when nothing moved.
u32 recentre_rings(const RingParams& params, i64 camera_x, i64 camera_z, RingLayout& layout);

// Which ring the point is in: the innermost whose square [c - half, c + half) holds it, so every
// point of the outer square is in exactly one ring. -1 outside the outer ring.
i32 ring_of(const RingLayout& layout, i64 x, i64 z) noexcept;

// Where a ring's heights come from: an `nx` x `nz` grid from (x0, z0) `spacing_mm` apart, µm,
// row-major with z rows. The dune field's (`FieldRingHeights`) is the one this module has; the
// renderer's, with its ridge rock and basins added, is a subclass of its own.
class RingHeights {
 public:
  virtual ~RingHeights() = default;
  virtual void heights(i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm, jobs::JobSystem* jobs,
                       Vector<i64>& out_um) const = 0;
};

class FieldRingHeights final : public RingHeights {
 public:
  FieldRingHeights(const DuneField& field, i64 time_us, Detail detail = Detail::dunes,
                   const LagField* lag = nullptr) noexcept
      : field_(field), time_us_(time_us), detail_(detail), lag_(lag) {}
  void heights(i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm, jobs::JobSystem* jobs,
               Vector<i64>& out_um) const override;

 private:
  const DuneField& field_;
  i64 time_us_;
  Detail detail_;
  const LagField* lag_;
};

// **A ring is built in chunks.** A ring's DAG built whole is one thread's work — 6.4 s for the
// inner ring's 2.1 million triangles, most of it the simplifier — and a re-centre would rebuild
// all of it. So a ring is cut into world-aligned chunks of `k_ring_chunk_cells` cells a side, each
// chunk's DAG is built on the job pool with its border vertices locked (so neighbouring chunks meet
// exactly at every level), and the ring's DAG is the chunks' merged in chunk order
// (`geometry::merge_cluster_lod`). A re-centre then rebuilds only the chunks whose cells, hole or
// border changed and keeps the rest; the merge is the same bytes either way. The price of the
// locks: a chunk's coarsest level keeps its border, a few hundred triangles, so a ring's far chunks
// cannot coarsen to one cluster each (terrain.md, "Rings", has the numbers).
inline constexpr i64 k_ring_chunk_cells = 128;

struct RingChunkCoord {
  i32 i = 0, j = 0;  // world chunk indices: [i, i + 1) * k_ring_chunk_cells * spacing, mm
};

// The chunks a ring's cells fall in, in chunk order (z rows, then x), leaving out a chunk wholly
// inside its hole.
void ring_chunks(const Ring& ring, Vector<RingChunkCoord>& out);
// What decides a chunk's mesh besides the heights: its cells, its hole and which of its sides are
// the ring's border. Equal keys, equal meshes.
u64 ring_chunk_key(const Ring& ring, RingChunkCoord chunk) noexcept;

// One chunk's mesh: its cells outside the hole, two counter-clockwise triangles a cell (seen from
// +y, the renderer's winding), then the skirts of the ring's border in it. Positions in metres,
// normals by central differences over a one-vertex apron (so a border vertex's normal is the
// ground's, not a one-sided guess), UVs in the terrain's frame, and `locked` set on the chunk's
// border, the hole's edge and every skirt vertex.
struct RingMesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u8> locked;
  Vector<u32> indices;
  u32 grid_vertices = 0;  // the first `grid_vertices` are the grid's; the rest are skirts'
  u32 grid_triangles = 0;
  u32 skirt_triangles = 0;
};

void build_ring_chunk_mesh(const Ring& ring, RingChunkCoord chunk, const RingParams& params,
                           const RingHeights& source, jobs::JobSystem* jobs, RingMesh& out);

struct RingChunk {
  RingChunkCoord coord;
  u64 key = 0;
  geometry::ClusterLodMesh lod;  // empty for a chunk with no triangles
  u32 grid_triangles = 0;
  u32 skirt_triangles = 0;
};

// One chunk's mesh and DAG, on the calling thread.
bool build_ring_chunk(const Ring& ring, RingChunkCoord chunk, const RingParams& params,
                      const RingHeights& source, const geometry::ClusterLodOptions& options,
                      RingChunk& out, std::string* error = nullptr);

// The rings round a moving camera: their layout, every ring's chunks, and every ring's merged DAG,
// kept current by the re-centre rule. Owns nothing but what it built; `source` must outlive it.
class TerrainRings {
 public:
  // Lays the rings out round the camera and builds every one.
  bool reset(const RingParams& params, i64 camera_x, i64 camera_z, const RingHeights& source,
             const geometry::ClusterLodOptions& options, jobs::JobSystem* jobs,
             std::string* error = nullptr);
  // The re-centre rule for the camera's new position: rebuilds what it says to (`rebuilt`, a mask
  // of rings) and nothing when it says nothing.
  bool update(i64 camera_x, i64 camera_z, jobs::JobSystem* jobs, u32& rebuilt,
              std::string* error = nullptr);
  // Rebuilds the rings in `mask` from the source as it is now (a new time, say), keeping every
  // chunk whose key is unchanged: pass `drop_chunks` to rebuild them all.
  bool rebuild(u32 mask, jobs::JobSystem* jobs, std::string* error = nullptr);
  void drop_chunks() noexcept {
    for (RingState& r : rings_)
      r.chunks.clear();
  }

  const RingLayout& layout() const noexcept { return layout_; }
  const RingParams& params() const noexcept { return params_; }
  u32 count() const noexcept { return layout_.count; }
  const geometry::ClusterLodMesh& lod(u32 ring) const noexcept { return rings_[ring].lod; }
  u64 hash(u32 ring) const noexcept { return rings_[ring].hash; }
  const Vector<RingChunk>& chunks(u32 ring) const noexcept { return rings_[ring].chunks; }
  // What the last reset, update or rebuild built and kept, in chunks.
  u32 last_built() const noexcept { return last_built_; }
  u32 last_reused() const noexcept { return last_reused_; }

 private:
  struct RingState {
    Vector<RingChunk> chunks;  // chunk order
    geometry::ClusterLodMesh lod;
    u64 hash = 0;
  };
  RingParams params_;
  RingLayout layout_;
  const RingHeights* source_ = nullptr;
  geometry::ClusterLodOptions options_;
  RingState rings_[k_max_rings];
  u32 last_built_ = 0;
  u32 last_reused_ = 0;
};

}  // namespace engine::terrain
