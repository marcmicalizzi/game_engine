#pragma once

// Tile building: triangles in, one tile of navigation mesh out (plan 05 §5.11).
//
// `build_tile` runs Recast's voxel pipeline — rasterize, filter, compact, erode, region,
// contour, polygonize, detail — and hands back a `NavTileData`, a self-contained byte buffer.
// It touches no global state, takes no locks, and allocates only its own scratch, so the
// rebuild queue can run one per worker without coordination.
//
// **What the bytes are.** A tile's bytes are the *polygon mesh*, not Detour's tile format.
// Detour's is produced from these by `NavMesh::add_tile`, because an off-mesh link has to be
// baked into a tile's Detour data and links are added and removed at run time (plan 05 §5.8:
// destruction opens passages). Keeping the polygon mesh as the stored form means one source of
// truth per tile: adding a link re-bakes from it instead of patching a blob, and the bytes a
// determinism test compares are the bytes the builder produced rather than a downstream
// encoding that also contains a bounding-volume tree.
//
// The format is described in docs/subsystems/nav.md. It is not a file format yet — it carries no
// endianness or alignment promises across machines — so it is a cache, never an asset.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/nav/types.h>

#include <span>

namespace engine::nav {

// --- build parameters -------------------------------------------------------------------------

// Everything the voxel pipeline needs. The agent dimensions and the cell size are the two that
// matter: cell size sets both the cost (quadratic in the tile's voxel count) and the precision
// of every wall, and the agent radius is *eroded* out of the walkable area, so a mesh built for
// a 0.6 m agent already keeps that agent's shoulders out of the wall and a path along it needs
// no radius correction.
struct NavBuildParams {
  // --- the agent the mesh is built for
  f32 agent_radius = 0.6f;
  f32 agent_height = 2.0f;
  f32 agent_max_climb = 0.4f;  // the tallest step the agent walks up without a link
  f32 agent_max_slope_degrees = 45.0f;

  // --- voxelization. cell_height is usually below cell_size: the vertical axis only has to
  // resolve steps and ledges, and halving it doubles the span count.
  //
  // **`tile_size` must be an exact multiple of `cell_size`** and `validate()` refuses a pair
  // that is not. The voxel grid spans `round(tile_size / cell_size)` cells, and Recast puts a
  // tile's border portals at voxel 0 and voxel N of *that* grid, so a tile size the cell size
  // does not divide leaves a seam — 64 m at 0.3 m cells covers 63.9 m — and Detour links tiles
  // by comparing portal coordinates to within a centimetre. The failure is silent: every tile
  // builds, every tile is added, and no path ever crosses a tile border.
  f32 cell_size = 0.25f;
  f32 cell_height = 0.2f;

  // --- the world grid (plan 05 §5.11: tiles are aligned to it, not to the geometry's bounds).
  // `origin` is the minimum corner of tile (0, 0) and `tile_size` the grid's pitch in x and z;
  // `height_min`/`height_max` bound the voxel column, and a tile whose geometry leaves that
  // range is clipped rather than grown, so two neighbouring tiles always agree on their shared
  // border.
  Vec3 origin{0.0f, 0.0f, 0.0f};
  f32 tile_size = 64.0f;
  f32 height_min = -64.0f;
  f32 height_max = 256.0f;

  // --- regions. Sizes are the side of a square in **world units**; the backend wants voxel
  // areas and the conversion lives in one place (src/tile_build.cpp) so a caller never has to
  // know the cell size to ask for "drop islands smaller than two metres across".
  f32 region_min_size = 2.4f;    // islands smaller than this are discarded
  f32 region_merge_size = 6.0f;  // regions smaller than this are merged into a neighbour
  // Watershed partitioning gives the best-shaped regions and is the default; monotone is faster
  // and never fails, at the cost of long thin regions that make paths wander. Layer
  // partitioning is not wrapped.
  bool monotone_regions = false;

  // --- contours and polygons
  f32 edge_max_len = 12.0f;   // longer border edges are subdivided; 0 disables
  f32 edge_max_error = 1.3f;  // how far a simplified contour may sit from the voxel edge
  u32 verts_per_poly = 6;     // Detour's limit is 6 and its cost is linear in this

  // --- detail mesh. Distances are multiples of cell_size and cell_height respectively, which
  // is Recast's convention and the only place in this struct that is not world units.
  //
  // The detail mesh refines the walkable surface's *height* inside each polygon. Without one a
  // polygon is a plane, so an agent on a long ramp reads the wrong ground height and a query
  // snaps to it; with one, the surface follows the voxels. A `detail_sample_dist` below 0.9 asks
  // for none, and then the stage is skipped outright and Detour interpolates height across the
  // polygon instead — the cheap end of the rebuild budget, and right for flat ground.
  f32 detail_sample_dist = 6.0f;
  f32 detail_sample_max_error = 1.0f;

  // --- span filters. All three are on in any configuration that wants a mesh an agent can
  // actually stand on; they are switches because a test that wants to see the unfiltered
  // voxelization has no other way to ask.
  bool filter_low_hanging_obstacles = true;
  bool filter_ledge_spans = true;
  bool filter_walkable_low_height_spans = true;
};

// True when the parameters describe a tile the backend can build. Every field that has to be
// positive, and the agent has to fit in the voxel grid.
bool validate(const NavBuildParams& params) noexcept;

// --- the built tile ---------------------------------------------------------------------------

// Per-region facts the region graph needs, read straight out of a tile's bytes. A region is a
// connected component of the tile's walkable polygons — not Recast's own region id, which can
// split one walkable area into several and does not promise connectivity.
struct TileRegionInfo {
  Aabb3 bounds{};
  Vec3 centroid{};  // area-weighted, so a long corridor's node sits in the middle of the corridor
  f32 area = 0.0f;  // square metres of walkable surface, projected on the horizontal plane
  u32 poly_count = 0;
};

// One stretch of a region's boundary that lies on a tile border, so the region graph can join it
// to whatever is on the other side. `side` is 0 = -x, 1 = +z, 2 = +x, 3 = -z (Detour's own
// order). `lo`/`hi` are the interval along the border axis — z for an x side, x for a z side —
// `axis` is the border's own coordinate, and the y range is what keeps the two floors of a
// stairwell that share a tile border from being joined into one region.
struct TilePortal {
  f32 lo = 0.0f;
  f32 hi = 0.0f;
  f32 y_min = 0.0f;
  f32 y_max = 0.0f;
  f32 axis = 0.0f;
  u16 region = 0;
  u8 side = 0;
  u8 pad = 0;
};

// The result of one build: an opaque byte buffer plus the handful of facts a caller needs
// without decoding it. Movable, never copied by accident.
class NavTileData {
 public:
  NavTileData() noexcept = default;
  ~NavTileData() = default;
  NavTileData(NavTileData&&) noexcept = default;
  NavTileData& operator=(NavTileData&&) noexcept = default;
  ENGINE_NON_COPYABLE(NavTileData);

  bool empty() const noexcept { return bytes_.empty(); }
  // A tile that built successfully but holds nothing walkable: legal, and the way a caller
  // removes a tile whose floor was destroyed.
  bool walkable() const noexcept { return poly_count_ != 0; }

  std::span<const u8> bytes() const noexcept { return {bytes_.data(), bytes_.size()}; }
  TileCoord coord() const noexcept { return coord_; }
  Aabb3 bounds() const noexcept { return bounds_; }
  u32 vert_count() const noexcept { return vert_count_; }
  u32 poly_count() const noexcept { return poly_count_; }
  u32 region_count() const noexcept { return region_count_; }

  // The grid this tile was built on, carried in its own bytes. Everything that consumes tiles —
  // the mesh, the region graph — reads the grid from the tile instead of being told it
  // separately, so a tile and its consumer cannot disagree about where the tile's corners are.
  Vec3 grid_origin() const noexcept { return grid_origin_; }
  f32 grid_tile_size() const noexcept { return grid_tile_size_; }

  // The agent this tile was built for, carried in its bytes for the same reason the grid is.
  f32 agent_height() const noexcept { return agent_height_; }
  f32 agent_radius() const noexcept { return agent_radius_; }
  f32 agent_climb() const noexcept { return agent_climb_; }

  // The region table and the border portals.
  std::span<const TileRegionInfo> regions() const noexcept {
    return {regions_.data(), regions_.size()};
  }
  std::span<const TilePortal> portals() const noexcept {
    return {portals_.data(), portals_.size()};
  }

  // An explicit copy, for a caller that wants to keep a tile it also handed to a NavMesh.
  NavTileData clone() const;

  // Rebuilds a tile from bytes `bytes()` produced. Returns false for a buffer this build does
  // not understand, which is how a cache entry from an older version is rejected rather than
  // misread.
  bool from_bytes(std::span<const u8> bytes);

 private:
  Vector<u8> bytes_;
  Vector<TileRegionInfo> regions_;
  Vector<TilePortal> portals_;
  Aabb3 bounds_{};
  Vec3 grid_origin_{};
  f32 grid_tile_size_ = 0.0f;
  f32 agent_height_ = 0.0f;
  f32 agent_radius_ = 0.0f;
  f32 agent_climb_ = 0.0f;
  TileCoord coord_{};
  u32 vert_count_ = 0;
  u32 poly_count_ = 0;
  u32 region_count_ = 0;
};

// What one build cost, for E11 and for the rebuild queue's telemetry.
struct TileBuildStats {
  u32 span_count = 0;
  u32 vert_count = 0;
  u32 poly_count = 0;
  u32 region_count = 0;
  u32 triangles_in = 0;
  u32 triangles_rasterized = 0;  // after the per-triangle bounds reject
};

// The triangle soup one tile is built from. `vertices` is world space and `indices` three per
// triangle; `areas` is one `Area` per triangle and may be empty, in which case every triangle
// steep enough to fail `agent_max_slope_degrees` becomes `Area::Null` and the rest
// `Area::Ground`. Nothing is kept after the call returns.
//
// **Hand over geometry past the tile's own square**, at least as far as `tile_bounds(params,
// coord, /*expanded=*/true)`. The agent radius is eroded off the walkable area, so a floor that
// stops exactly at the tile border leaves no polygon touching it, the tile gets no border
// portals, and it links to none of its neighbours. Nothing reports this: the tile builds, the
// tile is added, and no path ever leaves it.
struct TileGeometry {
  std::span<const Vec3> vertices;
  std::span<const u32> indices;
  std::span<const u8> areas;
};

// Builds one tile. The same parameters, coordinate, and geometry always produce the same bytes —
// Recast is single-threaded and index-ordered, and nothing here depends on an address or on the
// order jobs finished in — which is what lets a rebuild be redone on a different machine, or
// checked against a cache, instead of re-measured.
//
// Triangles outside the tile's column are skipped before rasterization, so a caller may hand the
// same soup to every tile of a region; a caller that can cheaply pre-cull should, because the
// reject is per triangle and not free.
Status build_tile(const NavBuildParams& params, TileCoord coord, const TileGeometry& geometry,
                  NavTileData& out, TileBuildStats* stats = nullptr);

// The tile's world bounds on the grid, without building anything. `expanded` adds the border the
// voxel pipeline needs on each side, which is what a caller pre-culling geometry has to use.
Aabb3 tile_bounds(const NavBuildParams& params, TileCoord coord, bool expanded = false) noexcept;

}  // namespace engine::nav
