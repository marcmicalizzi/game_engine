#pragma once

// Module-private: run a block against the world itself rather than against flecs' command queue.
//
// This is not an optimization, it is the difference between working and not. Inside a deferred
// region — which is every system body, and any `defer_begin()` a caller opened — `ensure()`
// returns a *temporary buffer* that flecs merges later, and an observer or a component created
// there does not exist until that merge, by which time the commands it was meant to see have
// already been applied. Both failures are silent, and both cost an afternoon to find. So every
// accessor in this module that reaches a per-world singleton runs undeferred, and the seams are
// therefore safe to call from a system body, which is exactly where a capability will call them.

#include <flecs.h>

namespace engine::ecs::detail {

class Undeferred {
 public:
  explicit Undeferred(flecs::world_t* world) noexcept
      : world_(world), suspended_(world != nullptr && ecs_is_deferred(world)) {
    if (suspended_) ecs_defer_suspend(world_);
  }
  ~Undeferred() {
    if (suspended_) ecs_defer_resume(world_);
  }
  Undeferred(const Undeferred&) = delete;
  Undeferred& operator=(const Undeferred&) = delete;
  Undeferred(Undeferred&&) = delete;
  Undeferred& operator=(Undeferred&&) = delete;

 private:
  flecs::world_t* world_ = nullptr;
  bool suspended_ = false;
};

}  // namespace engine::ecs::detail
