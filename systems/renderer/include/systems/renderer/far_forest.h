#pragma once

// **The far forest** (docs/subsystems/renderer.md, "The far forest"; ADR-0059; roadmap R111): a
// streamed world's own trees past what its ring draws, as their species' coarsest form — the
// whole-tree impostor star where the flora container has one (flora.md, "The impostor chain") —
// merged per world tile into one rigid mesh of a few clusters and drawn as one instance of the
// load's prefix per tile. A world tile is drawn by the ring's tail or by the far forest, never by
// both and never by neither: the tail's blocks are keyed by the world's tile, and every tile whose
// block the tail holds with instances in it has its far instance parked out of every view; every
// other tile's is at its corner. The hand-off is decided by the same block list the tail is laid
// out from and rides the same table flip, so it is exact in the frame the tail changes.
//
// What the load builds (scene.cpp, `stream_authored`, through `far_forest_build.h`) is carried in
// `SceneData::far_forest`; `SceneRenderer` owns a `FarForestHandOff` over it and asks it what moves
// whenever a tail is handed over.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/world.h>

namespace engine::renderer {

// The world capability's tile key (`world::tile_key`, systems/world/tile_ring.h), mirrored because
// the renderer may not depend on the capability: the tail's blocks are named by it (engine-view's
// `ViewWorld::hand_over`), and the far forest finds a tile's block by it. `far_forest_tests.cpp`
// holds this spelling to the world's bits.
constexpr u64 far_tile_key(i32 x, i32 z) noexcept {
  return (u64{static_cast<u32>(x) ^ 0x80000000u} << 32) | u64{static_cast<u32>(z) ^ 0x80000000u};
}

// One world tile's far mesh and its instance in the prefix.
struct FarForestTile {
  u64 key = 0;  // far_tile_key(x, z)
  i32 x = 0;    // the world tile, in tiles
  i32 z = 0;
  u32 mesh = 0;       // the scene mesh, one per tile
  u32 instance = 0;   // the loaded instance, in the prefix
  u32 trees = 0;      // instances merged into it
  u32 triangles = 0;  // its finest level's triangles
  WorldPos corner{};  // where it stands when drawn: the tile's corner at height zero
};

// What the load reports about the far forest (engine-view's summary, the tests).
struct FarForestStats {
  u32 tiles = 0;             // far tiles built (one mesh and one prefix instance each)
  u32 trees = 0;             // streamed instances merged into them
  u32 skipped_small = 0;     // streamed instances under `min_height_m`
  u32 skipped_heavy = 0;     // streamed instances whose coarsest form passes `max_triangles`
  u32 skipped_textures = 0;  // instances of a mesh whose far materials have no built textures
  u32 meshes = 0;            // scene meshes the far forest draws
  u64 triangles = 0;         // finest level, all tiles
  u64 clusters = 0;          // every level, all tiles: the prefix pairs the far forest takes
  u32 cache_hits = 0;        // far tiles read from the derived-data cache
  u32 cache_misses = 0;      // built (and written, when the load writes the cache)
  f64 build_ms = 0.0;        // wall time of the far forest's load stage
};

struct FarForestData {
  Vector<FarForestTile> tiles;  // sorted by key
  f32 tile_size = 0.0f;
  FarForestStats stats;
  bool empty() const noexcept { return tiles.empty(); }
};

// Where a far tile stands while the tail draws its tile: a million kilometres below its corner,
// where no view, no cascade and no camera ray reaches (renderer.md, "The far forest", "Parked, not
// hidden").
inline constexpr f64 k_far_park_depth_m = -1.0e9;

}  // namespace engine::renderer
