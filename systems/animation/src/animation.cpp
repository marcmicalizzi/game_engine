#include <systems/animation/animation.h>

namespace engine::animation {

void AnimationSystem::begin_tick(u64 tick) noexcept {
  tick_ = tick;
  elapsed_ = 0.0f;
}

void AnimationSystem::tick(f32 dt) noexcept {
  // TODO(animation): the capability's per-tick work. Iterate this capability's own storage in memory
  // order; no data-dependent branch inside the inner loop (plan 11 §11.3, §11.4).
  elapsed_ += dt;
}

void AnimationSystem::end_tick() noexcept {
  // TODO(animation): publish results and emit events.
}

u32 AnimationSystem::lod_tier(f32 observer_score) noexcept {
  // TODO(animation): the real tier boundaries and their hysteresis (plan 05 §5.4). Placeholder:
  // simulated near, dropped far, so the policy exists and is tested from the first commit.
  return observer_score < 1.0f ? 0u : 1u;
}

}  // namespace engine::animation
