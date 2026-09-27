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
#include <systems/renderer/view_set.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/world.h>
#include <systems/world/world_log.h>

#include <memory>
#include <string>

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
  // the renderer as its sink. `data` and `renderer` must outlive this.
  bool create(const renderer::SceneData& data, renderer::SceneRenderer& renderer,
              std::string* error);
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
  world::WorldLog log_;
  std::string log_path_;  // empty: no log
  std::string log_text_;
  world::PlacementTiles placements_;
  world::PlacementTilesConfig placements_config_;
  Vector<world::PlacementTiles::TileRange> ranges_;  // hand_over's: the tail's tiles
  Vector<renderer::DynamicBlock> blocks_;            // and the blocks they are
  bool compact_next_ = false;  // the next hand-over lays the tiles out from scratch (a restart)
};

}  // namespace engine::view

#endif  // ENGINE_VIEW_WORLD
