#pragma once

// The far forest's hand-off with the ring's tail (docs/subsystems/renderer.md, "The far forest";
// far_forest.h has what the load builds).

#include <systems/renderer/far_forest.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/tile_layout.h>

#include <span>

namespace engine::renderer {

// Holds which far tiles are drawn. `hand_over` takes the tail's blocks and says which far
// instances move and where; `commit` takes it once the move was accepted. A tile is drawn by the
// far forest unless a block named by its key (`far_tile_key`) holds instances: a tile the world
// holds but withholds (the pair cap's), and one it does not hold, are the far forest's. Allocates
// nothing after `reset`: every list is sized for every tile there.
class FarForestHandOff {
 public:
  // Every tile drawn at its corner, as the load placed them. A null or empty forest is inactive.
  void reset(const FarForestData* data);
  bool active() const noexcept { return data_ != nullptr && !data_->tiles.empty(); }
  // How many far instances the tail's `blocks` move; `indices()` and `placements()` name them.
  u32 hand_over(std::span<const DynamicBlock> blocks);
  std::span<const u32> indices() const noexcept { return {indices_.data(), indices_.size()}; }
  std::span<const SceneInstance> placements() const noexcept {
    return {placements_.data(), placements_.size()};
  }
  // The state `hand_over` computed is the drawn one (the move was accepted).
  void commit() noexcept;
  u32 drawn_count() const noexcept { return drawn_count_; }
  bool drawn(u32 tile) const noexcept { return tile < drawn_.size() && drawn_[tile] != 0; }

 private:
  const FarForestData* data_ = nullptr;
  Vector<u8> drawn_;   // per tile: 1 at its corner, 0 parked
  Vector<u8> wanted_;  // what `hand_over` computed
  Vector<u32> indices_;
  Vector<SceneInstance> placements_;
  u32 drawn_count_ = 0;
};

// Where a far tile's instance stands: at its corner when drawn, parked below the world otherwise.
SceneInstance far_tile_placement(const FarForestTile& tile, bool drawn) noexcept;

}  // namespace engine::renderer
