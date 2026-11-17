#pragma once

// **What stands in a line's water** (W138; channels.md, "What stands in the water"): the level of
// the solved channels' water over a place, so that `engine-content flow solve --under-water` lists
// the scene's instances standing in a line's pools and runs beside those standing in a body's (the
// forest's need 130: 26 instances newly in channel water after W130.2's pools, 22 of them on one
// line whose pools reach 49 m wide, which the list did not name).
//
// A place is in a block's water where it lies within the wet reach (`wet_left_m`, `wet_right_m`,
// each side of the line by its own sign) of a stretch the draw lays a sheet on
// (`scene_gen::channel_stretch_drawn`, and not a fall: a nappe is in the air), its level and reach
// interpolated along the stretch as the draw does (systems/water's `channel_at`). Stretches are
// found through a grid of `k_in_water_cell_m` squares, each holding the stretches whose reach
// touches it, so a query costs the few stretches near it and the index the blocks' stations.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>

#include <span>

namespace engine::channels {

inline constexpr f64 k_in_water_cell_m = 16.0;

class ChannelWater {
 public:
  // The blocks are read in place and must outlive the index.
  explicit ChannelWater(std::span<const scene::WaterChannel> blocks);
  // The level of the water over (x, z) and the block it is the water of: the first block (by its
  // index, then its stretch) whose sheet covers the place; false where none does.
  bool level_at(f64 x, f64 z, u32& block, f64& level) const;
  u32 stretches() const noexcept { return static_cast<u32>(items_.size()); }

 private:
  struct Item {
    u64 key;
    u32 block;
    u32 stretch;
  };
  std::span<const scene::WaterChannel> blocks_;
  Vector<Item> items_;  // by key, then block, then stretch
};

}  // namespace engine::channels
