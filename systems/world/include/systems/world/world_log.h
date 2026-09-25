#pragma once

// The world log (docs/subsystems/world.md, "The world log"): one `engine.world.TileFrame` per
// update — the tiles, the events, the ring's microseconds and each consumer's share — and one
// `engine.world.TileWorldSummary` for the run. Both hosts write it the same way (engine-view's
// `--world-log`, a headless run's result), so a number from either names the same thing, the way
// the renderer's flythrough records do.
//
// The consumers' shares are **this update's**: `ConsumerStats` are totals over the world's life,
// and the log takes a snapshot before the update and reports the difference.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <systems/world/world.h>

#include <schemas/world_tiles.h>

namespace engine::world {

class WorldLog {
 public:
  // Before an update: what every consumer's totals are now.
  void before(const World& world);
  // After it: the update as a frame record. `repeat`, `frame` and `recorded` are the caller's
  // (a flythrough's), and the renderer's columns (`instances`, `pairs`, the ruins') are left for
  // it.
  TileFrame after(const World& world, u32 repeat, u32 frame, bool recorded);
  // Everything since the log began, but the ruins consumer's own totals, which are the caller's.
  TileWorldSummary summary(const World& world) const;
  u64 updates() const noexcept { return ring_us_.size(); }

 private:
  Vector<ConsumerStats> before_;
  Vector<f64> ring_us_;
};

}  // namespace engine::world
