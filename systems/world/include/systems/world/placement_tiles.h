#pragma once

// The placements consumer (docs/subsystems/world.md, "The consumers"; scene_gen.md; ADR-0046; E34,
// E35, E37): a tile that activates gets what the scene's placement entries put on it — each entry's
// generator, found in the scene-generator registry by the name the scene gave it, asked for the
// tile (`scene_gen::PlacementGeneratorDesc::tile`) in the representation the tile's ring draws,
// which is exactly what the scene reader's whole expansion would have put there — as instances of
// the meshes the reader left resident; a tile that goes inactive drops them; a tile that moves
// between rings is built again when a generator draws the two rings differently. Once an update,
// the consumer hands the renderer every active tile's instances, **in tile order**, as the scene's
// tail (`renderer::SceneRenderer::set_dynamic_instances`). It names no generator: the ruins are the
// placement generator "ruins" (E34's blocks near and sections far is theirs), the city "city".
//
// **One consumer for every entry, in the scene's order.** A tile's placements are every entry's,
// entry by entry in the order the reader walks them (the `ruins` entries, then `placements`), so a
// tile streamed in is the whole read restricted to the tile; and the renderer takes one tail, so
// the pair budget is decided over all of it at once. Ground is not here: a world that holds its
// tiles' ground registers that consumer before this one (world.md, "The consumers"), and the
// placements stand on the terrain's ground function (the provider's floor), which is the same at
// any activation.
//
// **Nothing cross-fades a representation change**, so a building **pops** from sections to blocks
// when its tile crosses the inner radius, and E35 measures by how much; each placement's tag (the
// ruins': rubble) is kept beside the tail for engine-view's handover pass to split the pop by.
//
// **The pair budget is enforced here too.** A tile whose instances would take the scene past its
// budget — the visibility id's 2^24 pairs by default — is withheld at the commit, over every tile
// held: by ring, innermost first, then nearest first, then in tile order, and none after the first
// that does not fit. A withheld tile stays active and draws nothing until the budget has room.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

#include <memory>
#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::world {

// What a ring draws, the scene's per-ring word (`engine.scene.RingRuins`, a streamed world's
// `rings[r].ruins`): what the ruins generator draws a tile in, and what engine-view's handover pass
// and summary name a ring by.
enum class RingRuins : u8 { Blocks = 0, Sections = 1, Walls = 2 };
const char* ring_ruins_name(RingRuins ruins) noexcept;

// How many representations a generator may number its tiles' `kind` by, which the stats count
// (the ruins' are the scene's `RingRuins`: 0 blocks, 1 sections, 2 walls).
inline constexpr u32 k_placement_kinds = 4;

struct PlacementTilesConfig {
  f32 tile_size = 32.0f;  // the ring's; every streamed entry's grid must be the same
  u32 ring_count = 3;
  RingRuins ruins[k_max_rings] = {RingRuins::Blocks, RingRuins::Sections, RingRuins::Walls,
                                  RingRuins::Walls,  RingRuins::Walls,    RingRuins::Walls,
                                  RingRuins::Walls};
  // The scene's pair budget, its load's instances included.
  u32 max_pairs = renderer::k_max_pairs;
};

// Where the tail goes once an update. False with `error` is logged and counted, and the tail is
// offered again on the next change.
using InstanceSink = bool (*)(void* context, std::span<const renderer::SceneInstance> tail,
                              std::string* error);

struct PlacementTilesStats {
  // A generator's tile calls that put something on a tile, by the representation it drew (its own
  // numbering, `scene_gen::Placements::kind`), and their wall time.
  u64 built[k_placement_kinds] = {};
  i64 build_ns[k_placement_kinds] = {};
  u64 dropped = 0;   // tiles whose instances were dropped
  u64 refused = 0;   // tiles the pair budget withheld: each time one new or drawn is not drawn
  u64 failures = 0;  // generator or sink failures
  u64 handed = 0;    // tails handed to the sink
  i64 sink_ns = 0;   // the renderer taking the tail: the device wait and the tables' upload
  i64 max_sink_ns = 0;
  // Tiles built again for a moving ground (`restand`), and their wall time.
  u64 restood = 0;
  i64 restand_ns = 0;
  // Tiles generated for queries (`query_tile`: cache misses), and their wall time.
  u64 queried = 0;
  i64 query_ns = 0;
  // Now: the things (buildings) the drawn tiles hold, and the tail's instances and pairs; the tiles
  // held but not drawn, for the budget.
  u32 buildings = 0;
  u32 withheld = 0;
  u32 instances = 0;
  u32 pairs = 0;
};

class PlacementTiles {
 public:
  PlacementTiles();
  ~PlacementTiles();
  ENGINE_NON_COPYABLE(PlacementTiles);

  // Opens every streamed placement entry's generator (`SceneData::streamed`) against the ring's
  // grid and the scene's world block, standing on the scene's terrain. `scene` must outlive the
  // consumer: its terrain is the ground the placements stand on and its parts are what a tile's
  // pairs are counted from. False, with `error`, for a generator this executable does not carry, or
  // an entry its generator refuses (a kit that will not read, an entry on another grid).
  bool create(const renderer::SceneData& scene, const PlacementTilesConfig& config,
              std::string* error = nullptr);
  void set_sink(InstanceSink sink, void* context) noexcept {
    sink_ = sink;
    sink_context_ = context;
  }
  // The row to register, after the ground's: every ring (a ring's representation is the config's).
  TileConsumer consumer() noexcept;
  // The ring whose scores order the tiles of one ring when the pair budget cannot draw them all:
  // the nearest first. Without one, tile order.
  void set_ring(const TileRing* ring) noexcept { ring_ = ring; }

  // The tail as it was last handed on, in tile order.
  std::span<const renderer::SceneInstance> tail() const noexcept {
    return {tail_.data(), tail_.size()};
  }
  // Parallel to `tail()`: each instance's tag, as its generator gave it (the ruins' 1 is rubble — a
  // debris member of a building in sections, a fallen block of one in blocks — and 0 wall). What
  // engine-view's handover pass splits a pop by.
  std::span<const u8> tail_tags() const noexcept { return {tail_tags_.data(), tail_tags_.size()}; }
  const PlacementTilesStats& stats() const noexcept { return stats_; }
  const PlacementTilesConfig& config() const noexcept { return config_; }
  // Whether some streamed entry puts anything on `tile`.
  bool occupied(TileCoord tile) const noexcept;
  // The instances one active tile holds; empty for another tile.
  std::span<const renderer::SceneInstance> tile_instances(TileCoord tile) const noexcept;
  // Where each active tile's instances sit in the tail, in tile order: what a caller swapping one
  // tile's instances for others needs (engine-view's handover pass).
  struct TileRange {
    u64 key = 0;
    u32 first = 0;
    u32 count = 0;
  };
  void tile_ranges(Vector<TileRange>& out) const;
  // Whether the pair budget left an active tile's placements out of the tail at the last commit.
  bool withheld(TileCoord tile) const noexcept;
  // **A moving ground** (world.md, "The consumers"; scatter.md, "A moving ground"): the time the
  // host's surface stands at. When it is not the one the held tiles were stood on, up to `budget`
  // of them — innermost ring first, then nearest, then in tile order — are built again with the
  // surface at that time as `scene_gen::Context::stand`, the same places, turns and scales standing
  // on the sand where it now is, and the tail is handed on. Returns how many were built.
  u32 restand(f64 surface_time_s, u32 budget);
  f64 stand_time() const noexcept { return stand_time_; }

  // **A query's tile** (world.md "The consumers", "Placements answered from the scene"; apps.md
  // "The game contract"; roadmap P44): the tile's placements as a function of the scene alone — the
  // first `entries` streamed entries in the scene's order, in the **innermost ring's**
  // representation, standing on the scene's own ground at its own time — generated on the first ask
  // into a cache keyed by the tile and kept until `trim_query_tiles` drops it; a tile cached with
  // fewer entries than asked is generated again with more. **Never the streamed block**: a held
  // tile is in whatever ring it is in and re-stood on the moving sand a few tiles an update, so
  // what it holds depends on how the ring got there and when; this does not, so a module's
  // `nearest_placement` answers the same in a recording, its replay and a capture. Empty for a tile
  // no entry occupies. `entries_for` is how many entries a kind needs: through the last entry of
  // that generator, since a later entry is handed the earlier ones' footprints and none after.
  std::span<const renderer::SceneInstance> query_tile(TileCoord tile, u32 entries);
  u32 entries_for(std::string_view generator) const noexcept;
  // Drops the cached query tiles farther than `reach` tiles (Chebyshev) from `centre`, which is
  // what bounds the cache: (2 · reach + 1)² tiles at most.
  void trim_query_tiles(TileCoord centre, i32 reach);
  u32 query_tile_count() const noexcept { return query_tiles_.size(); }

 private:
  struct Entry;
  struct TileBlock {
    u8 ring = k_inactive;
    bool withheld = false;  // held, and left out of the tail by the pair budget
    u32 pairs = 0;
    u32 buildings = 0;
    Vector<renderer::SceneInstance> instances;
    Vector<u8> tags;  // parallel to `instances`
    f64 stood = 0.0;  // the surface time it was built standing on
  };

  static bool activate(void* context, const TileEvent& event);
  static bool change_ring(void* context, const TileEvent& event);
  static void deactivate(void* context, const TileEvent& event);
  static void commit(void* context);
  // `scene_ground`: stand on the scene's own ground at its own time, never the re-stood surface
  // (a query's tile). `entries`: how many of the entries to run, in the scene's order.
  scene_gen::Context context_for(const Entry& entry, u8 ring,
                                 bool scene_ground = false) const noexcept;
  bool build(TileCoord tile, u8 ring, TileBlock& out, u32 entries = ~0u, bool for_query = false);
  bool take(const TileEvent& event);
  void admit();
  // The query cache: a tile's instances and how many entries made them.
  struct QueryTile {
    u32 entries = 0;
    Vector<renderer::SceneInstance> instances;
  };
  FlatMap<u64, QueryTile> query_tiles_;
  TileBlock query_scratch_;

  const renderer::SceneData* scene_ = nullptr;
  PlacementTilesConfig config_;
  // The world block the generators see (`scene_gen::Context::world`): the config's rings, with
  // the scene's radii.
  scene::WorldRings world_rings_;
  std::unique_ptr<renderer::TerrainSampler> ground_;
  Vector<std::unique_ptr<Entry>> entries_;
  FlatMap<u64, TileBlock> tiles_;
  Vector<renderer::SceneInstance> tail_;
  Vector<u8> tail_tags_;
  u32 tail_pairs_ = 0;
  const TileRing* ring_ = nullptr;
  struct Rank {
    u8 ring = 0;
    f32 score = 0.0f;
    u64 key = 0;
    u32 index = 0;
  };
  Vector<Rank> ranks_;  // commit's scratch
  InstanceSink sink_ = nullptr;
  void* sink_context_ = nullptr;
  PlacementTilesStats stats_;
  // Each scene mesh's reach on the ground (`renderer::ground_radius`), for the footprints a later
  // entry is handed; and the footprints of the entries before the one being built, one tile's.
  Vector<f32> radius_of_;
  Vector<scene_gen::Footprint> earlier_;
  // The surface the tiles stand on now, when the host's ground has moved on from the scene's time.
  std::unique_ptr<renderer::TerrainSampler> stand_;
  f64 stand_time_ = 0.0;
};

}  // namespace engine::world
