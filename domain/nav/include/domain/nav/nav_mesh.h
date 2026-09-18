#pragma once

// nav: the engine's navigation mesh (plan 05 §5.8, §5.11, ADR-0027).
//
// Recast/Detour 1.6 is the backend and is invisible from here: the surface is core/math (Vec3,
// Aabb3), core/containers handles, `std::span`, and `Status`. Tiles are added, replaced, and
// removed at run time; queries are `find_nearest`, `find_path` with string pulling, and
// `raycast`; off-mesh links are added and removed at run time, which is how a destroyed wall or
// a new hole becomes a route without rebuilding the world (plan 05 §5.8's "downstream effects").
//
// Determinism stance (ADR-0027, ADR-0010): **hashed**. A NavMesh's contents are a function of
// the tiles handed to it and the order they were handed over, and every query is a pure function
// of the mesh and its arguments. The queue that produces tiles is the part with a scheduler in
// it, and it is deliberately the only part: `RebuildQueue::apply_ready` applies results in tile
// order on the caller's thread, so what reaches this class never depends on which worker
// finished first (see rebuild_queue.h).
//
// Not wrapped, deliberately: Detour's tile cache and its temporary obstacles, layered (multi-
// level) tiles, `dtNavMeshQuery`'s sliced A*, random point queries, local-neighbourhood and
// wall-distance queries, and serialization of a whole mesh. See docs/subsystems/nav.md.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/nav/tile.h>
#include <domain/nav/types.h>

#include <span>

namespace engine::nav {

class Crowd;

// --- off-mesh links -----------------------------------------------------------------------

// A point-to-point edge the surface does not provide: a jump across a gap, a ladder, a hole a
// collapse opened in a floor (plan 05 §5.8). Both ends are snapped to the nearest polygon within
// `radius`; an end with no polygon near it makes the link inert rather than an error, because a
// destruction event that opens a passage into a room with no mesh yet is a normal race.
struct OffMeshLink {
  Vec3 start{};
  Vec3 end{};
  f32 radius = 0.6f;
  u16 flags = k_flag_jump;
  Area area = Area::Jump;
  bool bidirectional = true;
};

// --- the mesh -------------------------------------------------------------------------------

struct NavMeshOptions {
  // The grid. These must match the `NavBuildParams` the tiles were built with; `add_tile`
  // refuses a tile whose bounds do not sit on this grid, because a mesh whose tiles disagree
  // about their corners produces paths that stop at tile borders and no error anywhere.
  Vec3 origin{0.0f, 0.0f, 0.0f};
  f32 tile_size = 64.0f;

  // Hard caps, allocated up front (ADR-0017: the limits that exist are visible, in one struct).
  // `max_tiles` and `max_polys_per_tile` are not independent: a 32-bit polygon reference is
  // salt | tile | polygon, and Detour needs at least 10 bits of salt, so
  // ceil(log2(max_tiles)) + ceil(log2(max_polys_per_tile)) must not exceed 22. The defaults
  // spend 10 + 12. `init` refuses a pair that does not fit and says so.
  u32 max_tiles = 1024;
  u32 max_polys_per_tile = 4096;

  // Query scratch, allocated once. `max_search_nodes` is A*'s node pool: a search that exhausts
  // it returns the best partial path rather than failing, so this is a quality knob, not a
  // limit. Detour caps it at 65535. `max_path_polys` is the polygon corridor A* writes before
  // string pulling, and it is not the same number as the caller's point buffer: a corridor of
  // four points can cross a hundred polygons.
  u32 max_search_nodes = 2048;
  u32 max_path_polys = 512;

  // How far `find_nearest` looks around a point when a caller does not say. Half extents, so
  // the default searches a 4 m x 8 m x 4 m box — wide enough to find the floor under a character
  // standing on a doorstep, narrow enough not to find the floor below.
  Vec3 search_half_extents{2.0f, 4.0f, 2.0f};

  u32 max_off_mesh_links = 256;
};

class NavMesh {
 public:
  NavMesh() noexcept;
  ~NavMesh();
  ENGINE_NON_COPYABLE(NavMesh);

  Status init(const NavMeshOptions& options);
  void shutdown() noexcept;
  bool initialized() const noexcept { return impl_ != nullptr; }
  const NavMeshOptions& options() const noexcept;

  // --- tiles
  // Adds the tile, replacing whatever was at its coordinate. The tile's bytes are copied: the
  // caller may free or rebuild its NavTileData immediately, and the mesh keeps what it needs to
  // re-bake the tile when an off-mesh link changes.
  //
  // A tile with no walkable polygons removes whatever was there and stores nothing, so
  // "the floor of this tile was destroyed" and "this tile is empty" are the same call.
  Status add_tile(const NavTileData& tile);
  bool remove_tile(TileCoord coord);
  bool contains_tile(TileCoord coord) const noexcept;
  u32 tile_count() const noexcept;
  u32 poly_count() const noexcept;
  // The stored polygon mesh of a tile, for a caller that wants to feed the region graph or
  // re-examine a tile without keeping its own copy. Empty for a tile that is not resident.
  std::span<const u8> tile_bytes(TileCoord coord) const noexcept;

  TileCoord tile_containing(Vec3 point) const noexcept;
  Aabb3 bounds() const noexcept;  // over the resident tiles

  // --- off-mesh links. Adding or removing one re-bakes the single tile that owns the link's
  // start point, which costs a `dtCreateNavMeshData` (a bounding-volume tree over the tile's
  // polygons) and no voxelization: about a hundredth of a rebuild. The link survives a rebuild
  // of its tile.
  Status add_off_mesh_link(const OffMeshLink& link, OffMeshLinkId& out);
  bool remove_off_mesh_link(OffMeshLinkId id);
  bool off_mesh_link(OffMeshLinkId id, OffMeshLink& out) const;
  u32 off_mesh_link_count() const noexcept;

  // --- queries
  // The closest polygon to `point` within `half_extents`, or the options' default box.
  bool find_nearest(Vec3 point, const PathFilter& filter, NavPoint& out) const;
  bool find_nearest(Vec3 point, Vec3 half_extents, const PathFilter& filter, NavPoint& out) const;

  // A string-pulled corridor from `from` to `to`, written into `corridor`. The first point is on
  // the mesh under `from` and the last under `to`; a corridor of two points is a straight walk.
  //
  // `Status::Partial` means the goal is not reachable and the corridor ends at the closest
  // polygon the search could reach — which is the answer an agent needs, not an error.
  // `Status::LimitReached` means the corridor did not fit; what did fit is a valid prefix.
  Status find_path(Vec3 from, Vec3 to, const PathFilter& filter, std::span<Vec3> corridor,
                   PathResult& out) const;

  // Walks the surface from `from` towards `to` and stops at the first wall. This is the cheap
  // test — no A*, no node pool — that answers "can I just walk there", and it is what a mover
  // uses to shorten a corridor as it goes.
  bool raycast(Vec3 from, Vec3 to, const PathFilter& filter, RaycastHit& out) const;

  // --- diagnostics
  u64 revision() const noexcept;  // bumped by every tile or link change; a cheap change check

 private:
  friend class Crowd;
  void* backend() const noexcept;  // the dtNavMesh, for Crowd only

  struct Impl;
  Impl* impl_ = nullptr;
};

// --- agents -----------------------------------------------------------------------------------

// A minimal wrap of DetourCrowd: local avoidance and path following for the agents that are
// close enough to need them. Separate from NavMesh because a game with no crowd should pay for
// none of it — no proximity grid, no path queue, no per-agent corridor (plan 11 §11.10).
//
// Determinism stance: **derived**. dtCrowd smooths velocities with a time step and replans
// asynchronously through its own path queue, so agent positions are not part of the sim hash;
// gameplay-relevant movement is a corridor from `NavMesh::find_path` advanced by the sim.
struct CrowdOptions {
  u32 max_agents = 128;         // the cap everything is sized by
  f32 max_agent_radius = 0.6f;  // the largest radius any agent will have
};

struct AgentDesc {
  Vec3 position{};
  f32 radius = 0.6f;
  f32 height = 2.0f;
  f32 max_speed = 3.5f;
  f32 max_acceleration = 8.0f;
  f32 separation_weight = 2.0f;
  bool avoid_obstacles = true;  // steer around static walls
  bool avoid_agents = true;     // steer around other agents
  bool optimize_visibility = true;
  u64 user_data = 0;  // the caller's entity id, carried back on every read
};

struct AgentState {
  Vec3 position{};
  Vec3 velocity{};
  Vec3 target{};
  u64 user_data = 0;
  bool has_target = false;
};

class Crowd {
 public:
  Crowd() noexcept;
  ~Crowd();
  ENGINE_NON_COPYABLE(Crowd);

  // The mesh must outlive the crowd and must already be initialized; tiles may be added and
  // removed afterwards, and agents whose corridor a change invalidated replan on the next
  // `update`.
  Status init(NavMesh& mesh, const CrowdOptions& options);
  void shutdown() noexcept;
  bool initialized() const noexcept { return impl_ != nullptr; }

  Status add_agent(const AgentDesc& desc, AgentId& out);
  bool remove_agent(AgentId id);
  bool contains(AgentId id) const noexcept;
  u32 agent_count() const noexcept;
  u32 max_agents() const noexcept;

  // Asks the agent to walk to the nearest polygon to `target`. Returns false for a stale id or a
  // target with no mesh near it.
  bool request_move(AgentId id, Vec3 target);
  bool stop(AgentId id);

  // One step. `dt` is the sim's, not a wall clock: the crowd reads no clock of its own.
  void update(f32 dt);

  bool agent_state(AgentId id, AgentState& out) const;
  // The batch read path: one pass over the caller's id array, no per-agent call. A stale id
  // leaves its slot untouched.
  void read_positions(std::span<const AgentId> ids, std::span<Vec3> out) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace engine::nav
