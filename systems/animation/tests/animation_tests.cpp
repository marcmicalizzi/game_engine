// Tests for the animation capability (ADR-0027). TODO(animation): the invariants listed on the docs page
// belong here, one test each, before the capability ships.
#include <systems/animation/animation.h>

#include <doctest/doctest.h>

#include <string_view>

using namespace engine;

TEST_CASE("animation system runs a tick") {
  animation::AnimationSystem system;
  system.begin_tick(7);
  system.tick(1.0f / 60.0f);
  system.end_tick();
  CHECK(system.tick_index() == 7u);
  CHECK(system.elapsed() == doctest::Approx(1.0f / 60.0f));
}

TEST_CASE("animation LOD policy never coarsens as the observer gets nearer") {
  // The tier is monotonic in the observer score (plan 05 §5.4); hysteresis, once it exists,
  // widens the boundaries but must not break this.
  CHECK(animation::AnimationSystem::lod_tier(0.0f) <= animation::AnimationSystem::lod_tier(10.0f));
  CHECK(animation::AnimationSystem::lod_tier(10.0f) <= animation::AnimationSystem::lod_tier(1000.0f));
}

TEST_CASE("animation declares a determinism stance") {
  const std::string_view determinism{animation::k_determinism};
  CHECK((determinism == "hashed" || determinism == "derived"));
}
