#pragma once

// The coarse connectivity graph: one node per walkable region of a tile, one edge per border
// portal between two of them (plan 05 §5.11, §5.4).
//
// **Why a second tier exists.** Plan 05 §5.6 puts 10^5 scheduled NPCs in a world, and §5.4 says
// only the LOD0/LOD1 ones get detailed simulation. A
// Detour `findPath` is a best-first search over polygons with a node pool, a heap, and a
// string-pulling pass; at a few microseconds each, 10^5 of them per tick is not a budget any
// machine has, and most of those queries exist only to answer "roughly how far, and is it even
// reachable" for a traveller who will be summarized rather than simulated. The region graph
// answers that question over a graph three to four orders of magnitude smaller — a few nodes per
// tile instead of a few hundred polygons — and answers it with the same connectivity, because it
// is derived from the same tiles.
//
// It is an **estimate**, and the pages say so in both directions: a LOD2/LOD3 traveller moves on
// `estimate_distance`, and the moment it is promoted to LOD1 it gets a real corridor from
// `NavMesh::find_path`. The two never disagree about *reachability* — a region edge exists only
// where a polygon edge does — but they do disagree about length, by the factor measured in
// docs/subsystems/nav.md.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/small_vector.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/nav/tile.h>
#include <domain/nav/types.h>

#include <span>

namespace engine::nav {

// One node: a connected component of one tile's walkable polygons.
struct RegionNode {
  Vec3 centroid{};
  Aabb3 bounds{};
  f32 area = 0.0f;  // square metres, projected
  TileCoord coord{};
  u16 component = 0;
  bool alive = false;
};

struct RegionGraphStats {
  u32 node_count = 0;
  u32 edge_count = 0;  // counted once per direction
  u32 tile_count = 0;
  u64 revision = 0;
};

// An index into the graph's node array. Stable while the owning tile is resident; a tile that is
// rebuilt invalidates its own indices and nothing else's.
inline constexpr u32 k_invalid_region = 0xFFFFFFFFu;

class RegionGraph {
 public:
  RegionGraph() noexcept = default;
  ~RegionGraph() = default;
  ENGINE_NON_COPYABLE(RegionGraph);

  void clear() noexcept;

  // Adds or replaces one tile's regions and re-links them to the four neighbours. This is the
  // whole synchronization story: a caller that applies a tile to a NavMesh applies it here in
  // the same breath, and the two can never drift, because both are functions of the same bytes.
  void set_tile(const NavTileData& tile);
  // Same, from the bytes a NavMesh already holds (`NavMesh::tile_bytes`), so a caller that did
  // not keep its NavTileData does not have to.
  bool set_tile_bytes(std::span<const u8> bytes);
  void remove_tile(TileCoord coord);
  bool contains_tile(TileCoord coord) const noexcept;

  // An off-mesh link joins two regions directly, whatever tiles they are in: this is how a
  // destruction-created passage reaches the coarse tier (plan 05 §5.8). `id` is the caller's —
  // `OffMeshLinkId::handle.to_u64()` if it came from a NavMesh — so the same link can be removed
  // again.
  bool add_link(u64 id, Vec3 from, Vec3 to);
  bool remove_link(u64 id);
  u32 link_count() const noexcept { return static_cast<u32>(links_.size()); }

  u32 node_count() const noexcept;  // live nodes, not the array's length
  u32 edge_count() const noexcept;
  std::span<const RegionNode> nodes() const noexcept { return {nodes_.data(), nodes_.size()}; }
  const RegionNode* node(u32 index) const noexcept;
  u32 neighbour_count(u32 index) const noexcept;
  u32 neighbour(u32 index, u32 slot) const noexcept;

  // The region a point is in: the tile's region whose bounds contain it, closest centroid wins
  // when several do. `k_invalid_region` when the point's tile is not resident.
  u32 find_region(Vec3 point) const noexcept;

  // A* over the regions, accumulating distance from portal to portal. False when the two points
  // are in no region, or in regions with no path between them — which is the answer a caller
  // wanted in the first place, since "unreachable" is the expensive thing to discover with
  // Detour.
  bool estimate_distance(Vec3 from, Vec3 to, f32& out) const;
  bool connected(Vec3 from, Vec3 to) const;

  RegionGraphStats stats() const noexcept;

 private:
  // A portal edge: the neighbour node and the world point a traveller crosses at. The cost is
  // not stored because it depends on where the traveller entered the region — which is the
  // difference between a corridor estimate and a centroid-to-centroid one.
  struct Edge {
    u32 node = 0;
    Vec3 point{};
  };

  struct TileEntry {
    Vector<u32> nodes;  // one per region of the tile, in the tile's own region order
    Vector<TilePortal> portals;
  };

  struct Link {
    u64 id = 0;
    Vec3 from{};
    Vec3 to{};
    Vec3 point{};
    u32 a = k_invalid_region;
    u32 b = k_invalid_region;
  };

  u32 allocate_node();
  void release_node(u32 index);
  void unlink_node(u32 index);
  void link_tile_to_neighbours(TileCoord coord);
  void link_border(TileCoord a, u8 side);
  void connect(u32 a, u32 b, Vec3 point);
  void disconnect(u32 a, u32 b);
  void relink_links_touching(TileCoord coord);

  Vector<RegionNode> nodes_;
  Vector<SmallVector<Edge, 4>> edges_;
  Vector<u32> free_nodes_;
  HashMap<u64, TileEntry> tiles_;
  Vector<Link> links_;
  // The grid, taken from the first tile handed over rather than configured separately, so a
  // graph and a mesh fed the same tiles cannot be told different grids.
  Vec3 grid_origin_{};
  f32 grid_tile_size_ = 0.0f;
  f32 grid_climb_ = 0.0f;
  u32 live_nodes_ = 0;
  u64 revision_ = 0;

  // A* scratch, reused across queries so an estimate allocates nothing in steady state. Mutable
  // because `estimate_distance` is logically const; the class is not thread-safe and says so.
  mutable Vector<f32> g_score_;
  mutable Vector<Vec3> entry_point_;
  mutable Vector<u64> visit_stamp_;
  mutable Vector<u64> closed_stamp_;
  mutable Vector<u64> heap_;  // (f32 bits << 32 | node), so a plain integer heap sorts it
  mutable u64 visit_epoch_ = 0;
};

}  // namespace engine::nav
