#pragma once

// Shared vocabulary of domain/nav: the status enum, the tile grid, areas and flags, and the
// filter every query takes. Split out of nav_mesh.h so that tile.h, rebuild_queue.h, and
// region_graph.h can name a TileCoord without dragging in the mesh.
//
// Nothing here (or anywhere else under include/) mentions Recast or Detour. The backend is an
// implementation detail of src/, for the same two reasons physics keeps Jolt out of its public
// headers: a header that leaked `dtPolyRef` would make every consumer a Detour consumer, and
// Detour's types change width with the build flags its library was compiled with
// (DT_POLYREF64), so exactly one set of translation units may see them.

#include <core/base/types.h>
#include <core/containers/slot_map.h>
#include <core/math/math.h>

#include <span>

namespace engine::nav {

// How a call failed. Ok is the only unqualified success; Partial is a success the caller has to
// notice, because the corridor it got does not end where it asked.
enum class Status : u8 {
  Ok,
  Partial,          // a path to the closest reachable point; the goal is not connected
  InvalidArgument,  // the caller's parameters or geometry cannot be built or queried
  NotFound,         // no polygon near a point, or a stale handle
  LimitReached,     // a configured cap (tiles, polys, path length, agents) is full
  Unsupported,      // a combination the backend does not implement
  BackendError,     // the backend refused and said why in the log
};

const char* status_name(Status status) noexcept;

// --- the tile grid ----------------------------------------------------------------------------

// A tile of the navigation mesh. `x` indexes world x and `y` indexes world **z**: the grid is
// the world's horizontal grid seen from above, and Detour calls its second axis y for the same
// reason. Tiles are aligned to that grid by construction — `NavBuildParams::origin` and
// `tile_size` are the grid, and `tile_containing()` is the only rule that maps a point to a
// tile, so a tile built by `build_tile` and a tile asked for by the rebuild queue can never
// disagree about where its corners are (plan 05 §5.11).
struct TileCoord {
  i32 x = 0;
  i32 y = 0;

  constexpr bool operator==(const TileCoord&) const noexcept = default;
  constexpr auto operator<=>(const TileCoord&) const noexcept = default;
};

// A key for hashing and for ordering a set of tiles deterministically. Two's-complement
// round-trip, so negative coordinates keep their order among themselves.
constexpr u64 tile_key(TileCoord coord) noexcept {
  return (static_cast<u64>(static_cast<u32>(coord.x)) << 32) | static_cast<u32>(coord.y);
}

constexpr TileCoord tile_from_key(u64 key) noexcept {
  return TileCoord{static_cast<i32>(static_cast<u32>(key >> 32)),
                   static_cast<i32>(static_cast<u32>(key))};
}

// The tile a world point belongs to, on a grid of `tile_size` starting at `origin`. Floor, not
// truncation: a point at x = -0.5 with a 64 m grid is in tile -1, not tile 0.
inline TileCoord tile_containing(Vec3 point, Vec3 origin, f32 tile_size) noexcept {
  const f32 tx = (point.x - origin.x) / tile_size;
  const f32 tz = (point.z - origin.z) / tile_size;
  return TileCoord{floor_to_int(tx), floor_to_int(tz)};
}

// --- areas and flags --------------------------------------------------------------------------

// The area a polygon belongs to. Recast carries one byte per polygon and Detour uses it to index
// the filter's cost table, so an area is "what kind of ground is this" and a flag is "may this
// agent use it". Null is 0 because Recast's own unwalkable marker is 0 and the two must agree.
enum class Area : u8 {
  Null = 0,
  Ground = 1,
  Water = 2,
  Door = 3,
  Jump = 4,
  Hazard = 5,
  Count = 6,
};

inline constexpr u32 k_area_count = static_cast<u32>(Area::Count);
// Detour's own ceiling (DT_MAX_AREAS). A game that wants more kinds than the six above extends
// this enum up to here; the filter is converted to the backend's 64-entry table on every query.
inline constexpr u32 k_max_areas = 64;

const char* area_name(Area area) noexcept;

// Polygon flags. The low bits are the engine's; a game owns everything above k_flag_game_first.
inline constexpr u16 k_flag_walk = 1u << 0;
inline constexpr u16 k_flag_swim = 1u << 1;
inline constexpr u16 k_flag_door = 1u << 2;
inline constexpr u16 k_flag_jump = 1u << 3;
// Set on a polygon that exists but must not be used: a closed door, a blocked passage, a region
// a designer switched off. Excluded by the default filter, so "disabled" needs no second mesh.
inline constexpr u16 k_flag_disabled = 1u << 15;
inline constexpr u16 k_flag_game_first = 1u << 8;

// The flags `build_tile` gives a polygon of each area. Kept in one place so that the tile
// builder and a caller that wants to reason about the result agree.
constexpr u16 default_flags_for(Area area) noexcept {
  switch (area) {
    case Area::Water: return k_flag_swim;
    case Area::Door: return static_cast<u16>(k_flag_walk | k_flag_door);
    case Area::Jump: return k_flag_jump;
    case Area::Hazard: return k_flag_walk;
    case Area::Ground: return k_flag_walk;
    case Area::Null:
    case Area::Count: break;
  }
  return 0;
}

// What a query may traverse and what it costs. A polygon passes when it has at least one
// `include_flags` bit and no `exclude_flags` bit; `area_cost` multiplies the distance through a
// polygon of that area, so hazard at 10 is a detour an agent takes only when there is no other
// way. Costs below 1 are refused by the backend's A* (they break the heuristic's admissibility)
// and are clamped when the filter is converted.
struct PathFilter {
  u16 include_flags = static_cast<u16>(~k_flag_disabled);
  u16 exclude_flags = k_flag_disabled;
  f32 area_cost[k_area_count] = {1.0f, 1.0f, 2.0f, 1.0f, 1.5f, 10.0f};
};
static_assert(k_area_count == 6, "PathFilter::area_cost lists one default per area");

// --- handles ----------------------------------------------------------------------------------

// SlotMap handles in strong wrappers, so an AgentId cannot be passed where an OffMeshLinkId
// belongs. A default-constructed handle is null and every lookup of it fails rather than
// aliasing slot 0.
struct OffMeshLinkId {
  SlotHandle handle{};
  constexpr bool is_null() const noexcept { return handle.is_null(); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr bool operator==(const OffMeshLinkId&) const noexcept = default;
  constexpr auto operator<=>(const OffMeshLinkId&) const noexcept = default;
};

struct AgentId {
  SlotHandle handle{};
  constexpr bool is_null() const noexcept { return handle.is_null(); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr bool operator==(const AgentId&) const noexcept = default;
  constexpr auto operator<=>(const AgentId&) const noexcept = default;
};

// --- query results ----------------------------------------------------------------------------

// A point on the mesh. `poly` is an opaque polygon reference — Detour's, widened to 64 bits so
// that turning DT_POLYREF64 on later does not change this struct — and 0 means "no polygon".
struct NavPoint {
  Vec3 position{};
  u64 poly = 0;

  constexpr bool valid() const noexcept { return poly != 0; }
};

// What `find_path` wrote into the caller's corridor buffer.
struct PathResult {
  u32 count = 0;         // points written, including both endpoints
  f32 length = 0.0f;     // the corridor's length in world units
  bool partial = false;  // the goal polygon was not reachable; this ends at the closest point
};

// A walkability ray along the surface. `fraction` of 1 with `hit` false means the ray reached
// its end without crossing a wall, which is how a mover decides it may skip the corridor.
struct RaycastHit {
  Vec3 position{};
  Vec3 normal{};
  f32 fraction = 1.0f;
  bool hit = false;
};

// The length of a corridor, for callers that keep one around.
f32 path_length(std::span<const Vec3> points) noexcept;

}  // namespace engine::nav
