#pragma once

// engine-view's streamed world (`--world`, or a scene file's `world` block;
// docs/subsystems/world.md and apps.md): the world capability's tile ring over the loaded scene,
// with the camera as its one observer, and its placements consumer handing the renderer each
// frame's tail of instances. Compiled only where the world capability is (ENGINE_VIEW_WORLD);
// engine-view without it refuses `--world` with a sentence, which the minimal build proves.
//
// **The update runs between frames**, before `begin_frame`, because the renderer takes a new tail
// only between frames (`SceneRenderer::set_dynamic_instances`): in a flythrough from the frame's
// own camera (`renderer::FlightOptions::before_frame`), in the windowed loop from the previous
// frame's — a frame of lag on a ring of 32 m tiles, which is below anything a moving camera can
// see.

#if ENGINE_VIEW_WORLD

#include <core/base/types.h>
#include <core/json/json_value.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/renderer/view_set.h>
#include <systems/world/drawn_tiles.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/world.h>
#include <systems/world/world_log.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::view {

class ViewWorld {
 public:
  ViewWorld() = default;
  ~ViewWorld();
  ViewWorld(const ViewWorld&) = delete;
  ViewWorld& operator=(const ViewWorld&) = delete;

  // The ring from the scene's world block (its defaults where it gave none), the budget from the
  // tunables, and the placements consumer over the scene's streamed entries (its ruins, its
  // placements: whatever generators the scene names, found in the scene-generator registry) with
  // the renderer as its sink. With `tiles` — the renderer draws the ground from the world's tiles
  // (renderer.md, "The ground from the world's tiles") — the drawn ground's consumer too, before
  // the placements, handing the tile set the tiles the ring holds; the ring is the one the tile set
  // was built for. `data`, `renderer` and `tiles` must outlive this.
  bool create(const renderer::SceneData& data, renderer::SceneRenderer& renderer,
              std::string* error, renderer::TerrainTileSet* tiles = nullptr);
  bool valid() const noexcept { return renderer_ != nullptr; }
  // A world log: one `engine.world.TileFrame` a line, and the summary last (`close_log`).
  bool open_log(const std::string& path, std::string* error);
  void close_log();

  // How an update treats the budget. `Budgeted` is a frame of a moving camera; `Complete` fills
  // the ring with no budget from where it is (a capture from a camera the ring has not followed);
  // `Restart` drops every tile first and then fills it — a flythrough's repeat, so every repeat
  // starts from the same world, since with hysteresis a ring's state depends on how it got there.
  enum class Mode : u8 { Budgeted, Complete, Restart };
  // One update from `camera`, the log line with it.
  bool update(const renderer::Camera& camera, u64 tick, Mode mode, u32 repeat, u32 frame,
              bool recorded, std::string* error);
  // The untimed handover pass (`--world-handover`): the path flown one frame at a time, and for
  // every tile an update moves to another representation, the frame drawn as the update left it and
  // with only that tile's instances put back as they were, and the two compared; one
  // `engine.world.TileHandover` a tile, written to `path`. Returns how many.
  bool fly_handovers(const renderer::CameraPath& camera_path, u32 frames, const std::string& path,
                     u32& handovers, std::string* error);

  world::World& world() noexcept { return world_; }
  world::PlacementTiles& placements() noexcept { return placements_; }
  // **The nearest placement of a kind** (a `--game` module's `nearest_placement`, game_runner.h):
  // the streamed entries' generator names are the kinds ("ruins", "scatter"), and the answer is the
  // nearest instance of one, over the ground, within `radius` of `from`, among the tiles the
  // placements consumer holds. False when nothing of the kind is held that near. The tiles round
  // the camera are the innermost ring's, built first by every update, so a few metres round the
  // walker is always answered from what the ground's generators make there.
  bool nearest_placement(std::string_view kind, const WorldPos& from, f64 radius, WorldPos& out,
                         f64& distance) const;
  // **A block appended to the tail** (game_props.h, the game's props): instances every hand-over
  // adds after the tiles' blocks, as one `DynamicBlock` under `key`, so the renderer keeps them in
  // a block of their own beside the tiles and rewrites it only when they changed. Null: none, and
  // the hand-over is exactly what it was. `instances` must outlive this.
  void set_extra_block(const Vector<renderer::SceneInstance>* instances, u64 key) noexcept {
    extra_ = instances;
    extra_key_ = key;
  }
  // Hands the tail over again as it stands — the placements' tiles and the extra block — for a
  // frame on which only the extra block changed and no tile did. Between frames only.
  bool hand_over_now(std::string* error);
  const world::DrawnTiles& drawn() const noexcept { return drawn_; }
  // Whether a ring change of `tile` changes what its placements are drawn as (and it has any).
  bool is_handover(const world::TileEvent& event) const noexcept;
  const char* drawn_as(u8 ring) const noexcept;
  // The run's world: the log's summary line, and the same object in engine-view's summary.
  world::TileWorldSummary summary() const;
  JsonValue summary_json() const;

 private:
  static bool hand_over(void* context, std::span<const renderer::SceneInstance> tail,
                        std::string* error);

  const renderer::SceneData* data_ = nullptr;
  renderer::SceneRenderer* renderer_ = nullptr;
  world::World world_;
  sim::ObserverSet observers_;  // the camera, one observer, kept between updates
  world::WorldLog log_;
  std::string log_path_;  // empty: no log
  std::string log_text_;
  world::PlacementTiles placements_;
  world::PlacementTilesConfig placements_config_;
  Vector<std::string> kinds_;  // the streamed entries' generator names
  Vector<u32> mesh_kind_;      // per scene mesh, its index in `kinds_` (~0: placed by none)
  world::DrawnTiles drawn_;    // the drawn ground, when the renderer draws the world's tiles
  Vector<world::PlacementTiles::TileRange> ranges_;  // hand_over's: the tail's tiles
  Vector<renderer::DynamicBlock> blocks_;            // and the blocks they are
  bool compact_next_ = false;  // the next hand-over lays the tiles out from scratch (a restart)
  const Vector<renderer::SceneInstance>* extra_ = nullptr;  // the appended block's instances
  u64 extra_key_ = 0;
  Vector<renderer::SceneInstance> combined_;  // the tiles' tail and the extra block, kept
};

}  // namespace engine::view

#endif  // ENGINE_VIEW_WORLD
