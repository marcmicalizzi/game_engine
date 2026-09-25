#pragma once

// The ruins consumer (docs/subsystems/world.md, "The consumers"; ruins.md; E34, E35): a tile that
// activates gets its ruined building — assembled from the kit by `ruins::Assembler`, or laid block
// by block by `ruins::BlockAssembler`, exactly as the scene reader would have at load, from the
// same seed, tile and wind — as instances of the kits' meshes; a tile that goes inactive drops
// them; a tile that moves between rings is drawn again in the other ring's representation. Once an
// update, the consumer hands the renderer every active tile's instances, **in tile order**, as the
// scene's tail (`renderer::SceneRenderer::set_dynamic_instances`).
//
// **Blocks near, sections far, decided per tile** — E34's answer to the handover. Which
// representation a ring draws is the scene's (`engine.scene.WorldRings`), by default blocks in the
// inner ring, sections with their debris in the middle one and walls only beyond, the last being
// `ruins::detail_for_distance`'s far tier. Both representations of a seed are the same ruin — the
// block layer lays the section assembler's own footprint, ruin and openings (ruins.md) — which is
// what lets a tile change representation at a ring boundary without the building moving. It is not
// what makes the change invisible: nothing cross-fades two instance sets, so a building **pops**
// from sections to blocks when its tile crosses the inner radius, and E35 measures by how much.
//
// **The pair budget is enforced here too.** A tile whose instances would take the scene past its
// budget — the visibility id's 2^24 pairs by default — is refused before the renderer is asked, and
// counted; the tile stays active and draws nothing of its ruins until it moves rings or the budget
// has room. A world's pair count is bounded by the tiles round its observers, which is the point.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

#include <memory>
#include <span>
#include <string>

namespace engine::world {

// What the ruins are drawn as in one ring (`engine.scene.RingRuins`).
enum class RingRuins : u8 { Blocks = 0, Sections = 1, Walls = 2 };
const char* ring_ruins_name(RingRuins ruins) noexcept;

struct RuinsTilesConfig {
  f32 tile_size = 32.0f;  // the ring's; every streamed entry's must be the same
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

struct RuinsTilesStats {
  u64 assembled = 0;  // buildings assembled in sections (or walls)
  u64 laid = 0;       // buildings laid in blocks
  u64 dropped = 0;    // tiles whose instances were dropped
  u64 refused = 0;    // tiles the pair budget withheld: each time one new or drawn is not drawn
  u64 failures = 0;   // assembler or sink failures
  u64 handed = 0;     // tails handed to the sink
  i64 assemble_ns = 0;
  i64 lay_ns = 0;
  i64 sink_ns = 0;  // the renderer taking the tail: the device wait and the tables' upload
  i64 max_sink_ns = 0;
  // Now: the tiles holding a building, and the tail's instances and pairs; the tiles held but not
  // drawn, for the budget.
  u32 buildings = 0;
  u32 withheld = 0;
  u32 instances = 0;
  u32 pairs = 0;
};

class RuinsTiles {
 public:
  RuinsTiles();
  ~RuinsTiles();
  ENGINE_NON_COPYABLE(RuinsTiles);

  // Reads every streamed ruins entry's kits and chooses its buildings' tiles. `scene` must outlive
  // the consumer: its terrain is the ground the buildings stand on and its parts are what a tile's
  // pairs are counted from. False, with `error`, for a kit that will not read or an entry on
  // another grid.
  bool create(const renderer::SceneData& scene, const RuinsTilesConfig& config,
              std::string* error = nullptr);
  void set_sink(InstanceSink sink, void* context) noexcept {
    sink_ = sink;
    sink_context_ = context;
  }
  // The row to register: every ring (a ring's representation is the config's).
  TileConsumer consumer() noexcept;
  // The ring whose scores order the tiles of one ring when the pair budget cannot draw them all:
  // the nearest first. Without one, tile order.
  void set_ring(const TileRing* ring) noexcept { ring_ = ring; }

  // The tail as it was last handed on, in tile order.
  std::span<const renderer::SceneInstance> tail() const noexcept {
    return {tail_.data(), tail_.size()};
  }
  // Parallel to `tail()`: 1 where the instance is rubble — a debris member of a building in
  // sections, a fallen block of one in blocks — and 0 where it is wall. What engine-view's handover
  // pass splits a pop by.
  std::span<const u8> tail_rubble() const noexcept {
    return {tail_rubble_.data(), tail_rubble_.size()};
  }
  const RuinsTilesStats& stats() const noexcept { return stats_; }
  // Whether a streamed entry puts a building on `tile`.
  bool has_building(TileCoord tile) const noexcept;
  // The instances one active tile holds, and in which ring it drew them; empty for another tile.
  std::span<const renderer::SceneInstance> tile_instances(TileCoord tile) const noexcept;
  // Where each active tile's instances sit in the tail, in tile order: what a caller swapping one
  // tile's instances for others needs (engine-view's handover pass).
  struct TileRange {
    u64 key = 0;
    u32 first = 0;
    u32 count = 0;
  };
  void tile_ranges(Vector<TileRange>& out) const;
  // Whether the pair budget left an active tile's ruins out of the tail at the last commit.
  bool withheld(TileCoord tile) const noexcept;

 private:
  struct Entry;
  struct TileBlock {
    u8 ring = k_inactive;
    bool withheld = false;  // held, and left out of the tail by the pair budget
    u32 pairs = 0;
    u32 buildings = 0;
    Vector<renderer::SceneInstance> instances;
    Vector<u8> rubble;  // parallel to `instances`
  };

  static bool activate(void* context, const TileEvent& event);
  static bool change_ring(void* context, const TileEvent& event);
  static void deactivate(void* context, const TileEvent& event);
  static void commit(void* context);
  bool build(TileCoord tile, u8 ring, TileBlock& out);
  bool take(const TileEvent& event);
  void admit();

  const renderer::SceneData* scene_ = nullptr;
  RuinsTilesConfig config_;
  Vector<std::unique_ptr<Entry>> entries_;
  std::unique_ptr<renderer::TerrainSampler> ground_;
  FlatMap<u64, TileBlock> tiles_;
  Vector<renderer::SceneInstance> tail_;
  Vector<u8> tail_rubble_;
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
  RuinsTilesStats stats_;
};

}  // namespace engine::world
