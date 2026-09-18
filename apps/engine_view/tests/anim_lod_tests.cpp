// The app-side animation-LOD glue (`apps/engine_view/anim_lod.h`): the observer set built from a
// `renderer::ViewSet`, what each view is worth, what the camera says an instance is worth, and the
// band scale. It is deliberately testable without a GPU, a world or a clip — the glue is
// arithmetic over positions and frusta, and the things it hands to `sim::TierAssignment` and to
// `animation::AnimationSystem` are checked on the other side of those seams by their own modules
// (`domain/sim`'s hysteresis and rate-limit cases, `systems/animation`'s "a promoted instance holds
// the pose it would have had").
//
// The case that matters most here is the last one: a crowd walking across a band boundary must not
// thrash. The hysteresis and the rate limits that make that true are `sim::TierAssignment`'s, but
// *using* them correctly is this file's — a glue that rebuilt the tier array every frame, or that
// fed scores instead of importance, would pass every other case here and oscillate in a frame.
#include "../anim_lod.h"

#include <domain/sim/tiers.h>
#include <systems/animation/animation.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;

namespace {

// A `ViewSet` over a target, updated from one camera. No device: `build` and `update` are pure.
renderer::ViewSet make_views(renderer::ViewLayout layout, f32 peripheral_lod, u32 width, u32 height,
                             const renderer::Camera& camera) {
  renderer::ViewSetDesc desc;
  desc.layout = layout;
  desc.peripheral_lod = peripheral_lod;
  renderer::ViewSet views;
  std::string error;
  REQUIRE_MESSAGE(views.build(desc, width, height, &error), error);
  views.update(camera);
  return views;
}

renderer::Camera looking_down_z(f32 distance) {
  renderer::Camera camera;
  camera.position = Vec3{0.0f, 0.0f, distance};
  camera.target = Vec3{};
  return camera;
}

}  // namespace

TEST_CASE("anim lod: a view's weight is its LOD scale, and the set has one observer per view") {
  const renderer::Camera camera = looking_down_z(10.0f);
  const renderer::ViewSet single = make_views(renderer::ViewLayout::Single, 1.0f, 640, 480, camera);
  CHECK(single.size() == 1);
  CHECK(view::view_weight(single[0]) == doctest::Approx(1.0f));

  // A surround with peripheral LOD 4: the centre monitor is the attention region and keeps 1, the
  // sides are worth a quarter. That is `ViewQuality::lod_scale` and not a second knob — a player
  // who said the sides are worth a quarter of the geometric detail said it about the animation too.
  const renderer::ViewSet surround =
      make_views(renderer::ViewLayout::Surround3, 4.0f, 1920, 480, camera);
  REQUIRE(surround.size() == 3);
  CHECK(view::view_weight(surround[1]) == doctest::Approx(1.0f));   // centre
  CHECK(view::view_weight(surround[0]) == doctest::Approx(0.25f));  // left
  CHECK(view::view_weight(surround[2]) == doctest::Approx(0.25f));  // right
  CHECK(view::max_view_weight(surround) == doctest::Approx(1.0f));

  sim::ObserverSet observers;
  view::build_observers(surround, camera, observers);
  CHECK(observers.size() == 3);
  for (u32 o = 0; o < observers.size(); ++o) {
    // Today every view shares one eye, which is a property worth pinning: the day a layout has two
    // (split screen, a headset) this set stops being degenerate and nothing above it changes.
    CHECK(observers.position(o).z == doctest::Approx(camera.position.z));
  }
  CHECK(observers.weight(1) > observers.weight(0));
}

TEST_CASE("anim lod: importance is the best view that sees an instance, and 1/16 off screen") {
  const renderer::Camera camera = looking_down_z(10.0f);
  const renderer::ViewSet surround =
      make_views(renderer::ViewLayout::Surround3, 4.0f, 1920, 480, camera);

  // Straight ahead, far to the side, and behind the camera. The side point is chosen by asking the
  // views themselves rather than by guessing an angle: whatever the surround's frusta are, a point
  // only the left one contains has to come back with the left view's weight.
  const Vec3 ahead{0.0f, 0.0f, 0.0f};
  const Vec3 behind{0.0f, 0.0f, 40.0f};
  Vec3 side{-6.0f, 0.0f, 0.0f};
  const Frustum centre = frustum_from_view_proj(surround[1].view_proj);
  const Frustum left = frustum_from_view_proj(surround[0].view_proj);
  for (u32 step = 0; step < 64 && (frustum_contains_sphere(centre, side, 0.0f) ||
                                   !frustum_contains_sphere(left, side, 0.0f));
       ++step) {
    side.x -= 0.5f;
  }
  REQUIRE(frustum_contains_sphere(left, side, 0.0f));
  REQUIRE_FALSE(frustum_contains_sphere(centre, side, 0.0f));

  const Vec3 positions[3] = {ahead, side, behind};
  const f32 radii[3] = {0.0f, 0.0f, 0.0f};
  f32 importance[3] = {};
  view::view_importance(surround, std::span<const Vec3>(positions, 3),
                        std::span<const f32>(radii, 3), std::span<f32>(importance, 3));
  CHECK(importance[0] == doctest::Approx(1.0f));                          // the centre sees it
  CHECK(importance[1] == doctest::Approx(0.25f));                         // only a side monitor
  CHECK(importance[2] == doctest::Approx(view::k_offscreen_importance));  // no view at all

  // The radius is the conservative half: an instance whose centre is outside a frustum but whose
  // padded bounds reach into it is on screen, which is the direction a skinned limb needs.
  const Vec3 outside[1] = {behind};
  const f32 huge[1] = {60.0f};
  f32 padded[1] = {};
  view::view_importance(surround, std::span<const Vec3>(outside, 1), std::span<const f32>(huge, 1),
                        std::span<f32>(padded, 1));
  CHECK(padded[0] > view::k_offscreen_importance);
}

TEST_CASE("anim lod: the scale multiplies the capability's bands and 0 coarsens everything") {
  const sim::TierParams base = animation::tier_params();
  const sim::TierParams doubled = view::scaled_tier_params(base, 2.0f);
  for (u32 t = 0; t + 1 < base.tier_count; ++t)
    CHECK(doubled.boundaries[t] == doctest::Approx(base.boundaries[t] * 2.0f));
  // Everything else about the policy is the capability's and must come through untouched: the
  // hysteresis band and the rate limits are what keep a crowd from thrashing, and a scale that
  // quietly dropped them would make every other case here pass and the last one fail.
  CHECK(doubled.hysteresis == base.hysteresis);
  CHECK(doubled.max_promotions == base.max_promotions);
  CHECK(doubled.max_demotions == base.max_demotions);
  CHECK(doubled.tier_count == base.tier_count);

  const sim::TierParams none = view::scaled_tier_params(base, 0.0f);
  CHECK(sim::TierAssignment::tier_of(0.001f, none) == base.tier_count - 1);
}

TEST_CASE("anim lod: a crowd walking across a boundary does not thrash") {
  // 512 characters on a line, walked past the camera and back again over 400 ticks, so every one
  // of them crosses every band boundary twice. What is asserted is what makes the policy usable
  // rather than merely correct: the number of tier changes in one tick is bounded by the rate
  // limits, and the number of changes *per instance over the whole walk* is bounded by the
  // boundaries it actually crossed — which is hysteresis doing its job. Without the band, an
  // instance sitting on a boundary changes tier on most ticks.
  constexpr u32 k_instances = 512;
  constexpr u32 k_ticks = 400;
  const renderer::Camera camera = looking_down_z(10.0f);
  const renderer::ViewSet views = make_views(renderer::ViewLayout::Single, 1.0f, 1280, 720, camera);
  const sim::TierParams params = view::scaled_tier_params(animation::tier_params(), 1.0f);

  Vector<Vec3> positions(k_instances);
  Vector<f32> radii(k_instances, 0.5f);
  Vector<f32> importance(k_instances, 1.0f);
  Vector<u8> tiers(k_instances, u8{0});
  Vector<u32> flips(k_instances, 0u);
  Vector<sim::TierChange> changes;
  sim::TierAssignment assignment;

  u32 worst_tick = 0;
  for (u32 tick = 0; tick < k_ticks; ++tick) {
    // A slow sweep towards the camera and away again: every instance spends several ticks within a
    // whisker of each boundary, which is exactly where an unhysteretic policy oscillates.
    const f32 phase = static_cast<f32>(tick) / static_cast<f32>(k_ticks);
    const f32 depth = 200.0f * (1.0f - std::fabs(2.0f * phase - 1.0f));
    for (u32 i = 0; i < k_instances; ++i) {
      const f32 spread = static_cast<f32>(i) * 0.05f;
      positions[i] = Vec3{0.0f, 0.0f, camera.position.z - depth - spread};
    }
    view::view_importance(views, std::span<const Vec3>(positions.data(), positions.size()),
                          std::span<const f32>(radii.data(), radii.size()),
                          std::span<f32>(importance.data(), importance.size()));
    sim::TierInput input;
    input.positions = {positions.data(), positions.size()};
    input.importance = {importance.data(), importance.size()};
    input.tiers = {tiers.data(), tiers.size()};
    changes.clear();
    assignment.assign_tiers(
        input,
        [&] {
          sim::ObserverSet observers;
          view::build_observers(views, camera, observers);
          return observers;
        }(),
        params, changes);
    worst_tick = changes.size() > worst_tick ? static_cast<u32>(changes.size()) : worst_tick;
    // The rate limits are a promise about how much materialization one tick can be asked for.
    CHECK(changes.size() <= params.max_promotions + params.max_demotions);
    for (const sim::TierChange& change : changes)
      ++flips[change.index];
  }

  u32 worst_instance = 0;
  u64 total = 0;
  for (const u32 f : flips) {
    worst_instance = f > worst_instance ? f : worst_instance;
    total += f;
  }
  MESSAGE("crowd of " << k_instances << " over " << k_ticks << " ticks: " << total
                      << " tier changes, at most " << worst_instance
                      << " for one instance, at most " << worst_tick << " in one tick");
  // Three boundaries, crossed in and out again: six crossings is the truth, and the rate limits
  // can defer a crossing to a later tick but never turn one into two. Twelve is that with room for
  // the sweep's turning point sitting on a boundary; an unhysteretic policy scores in the hundreds.
  CHECK(worst_instance <= 12);
  CHECK(worst_instance >= 2);  // or the walk never crossed anything and this proves nothing
}
