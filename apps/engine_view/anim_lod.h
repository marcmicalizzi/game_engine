#pragma once

// **The camera decides how much a character is worth animating** — the app-side half of it
// (docs/subsystems/animation.md, "The app-side LOD glue"; docs/plan/05-simulation.md §5.4).
//
// `systems/animation` has had an LOD policy since it was built: four tiers, bands from
// `animation::tier_params()`, and materialization hooks that release a pose-pool slot on demotion
// and advance the playhead analytically on promotion. Its bench says a 5/15/30/50 tier mix costs
// **5.6×** less than the same population at LOD0. Nothing drove it. `engine-view --animate`
// attached every instance at LOD0 and left it there, so 1,024 foxes cost 3.1 ms of CPU a frame for
// a crowd of which most is a smudge.
//
// **Why this is a header in the app and not a function in either module.** The tier is
// `sim::TierAssignment`'s, the pose is `systems/animation`'s, and where the cameras are is
// `systems/renderer`'s — and the renderer must not depend on the animation capability, on
// `domain/ecs` or on flecs, while `domain/sim` must not depend on the renderer. The one place the
// three meet is the host that owns all three, which is this app. What crosses each boundary is
// data: a `sim::ObserverSet` and two spans in, a `sim::TierChange` list out, and
// `AnimationSystem::set_tier(Id128, u8)` to apply them. A game writes exactly this, which is why
// it is a header with its own tests rather than forty lines inside `main`.
//
// Nothing here includes flecs, a world, or a pose: it is arithmetic over positions and frusta, so
// the tests below drive it with no GPU and no `SimWorld`.

#include <core/base/types.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/sim/tiers.h>
#include <systems/renderer/view_set.h>

#include <span>

namespace engine::view {

// What an instance no view can see is worth. It divides the distance, so 1/16 makes an off-screen
// character score sixteen times as far away as it is — past `animation.lod.far` (120 m of observer
// score) for anything but a character standing on the camera, which is the intent: **off screen is
// tier 3**, the playhead frozen and the pool slot given back.
//
// Sixteen rather than infinity, and the difference matters. A score of infinity would pin an
// off-screen instance at the coarsest tier however close it is; a factor lets a character two
// metres behind the camera — about to be turned back towards — stay at tier 2 and keep its pose
// for the foot plant a gameplay query may ask about. It is also what keeps the rule *conservative*
// in the right direction when the frustum test is wrong: a frustum test that misses by a pixel
// costs a tier, not a pop from frozen.
inline constexpr f32 k_offscreen_importance = 1.0f / 16.0f;

// The weight of one view as an observer. The centre view of a surround is what the player is
// looking at and the side monitors are peripheral vision, which is the same thing
// `ViewQuality::lod_scale` already says about geometry: `--peripheral-lod 4` lets a cluster be four
// times as wrong in screen space out there. Reusing it rather than adding a second knob is
// deliberate — a player who has said the sides are worth a quarter of the detail has said it about
// the animation too, and two knobs that mean the same thing drift apart.
//
// The weight *divides* the distance in the tier score, so a view worth less makes what only it
// sees score farther away and coarsen sooner.
inline f32 view_weight(const renderer::View& view) noexcept {
  const f32 scale = view.quality.lod_scale > 0.0f ? view.quality.lod_scale : 1.0f;
  return 1.0f / scale;
}

// One observer per view, at that view's eye, with that view's weight
// (`sim::ObserverSet`, docs/subsystems/sim.md "Why tiers are a function over the observer set").
//
// **Today every view of a `ViewSet` shares one eye**, so the set is degenerate and the minimum
// over it is the distance divided by the largest weight. That is deliberately not worked around:
// the set is the shape the simulation's interest management already has, a split-screen or co-op
// layout fills it with two genuinely different eyes and everything below keeps working, and what a
// *view* is worth reaches the entity through its importance instead (`view_importance` below),
// which is exactly what importance is for. Writing it as one observer at the camera would have
// been shorter and would have been the wrong thing for a game to copy.
//
// The eye goes in as the camera holds it, a `WorldPos` (ADR-0053): the tier is the same 10,000 km
// out as by the origin.
inline void build_observers(const renderer::ViewSet& views, const renderer::Camera& camera,
                            sim::ObserverSet& out) {
  out.clear();
  for (u32 v = 0; v < views.size(); ++v)
    out.add(camera.position, view_weight(views[v]));
}

// The largest weight in the set built above, which `view_importance` normalizes against so that an
// instance the attention view can see has importance 1 and scores its plain distance.
inline f32 max_view_weight(const renderer::ViewSet& views) noexcept {
  f32 best = 0.0f;
  for (u32 v = 0; v < views.size(); ++v) {
    const f32 weight = view_weight(views[v]);
    best = weight > best ? weight : best;
  }
  return best > 0.0f ? best : 1.0f;
}

// What the camera says one instance is worth, as a `sim::TierInput::importance` factor: the best
// weight among the views whose frustum contains its padded bounds, over the best weight in the set,
// or `k_offscreen_importance` when no view contains it.
//
// **Why the frustum test is on the CPU and against a padded sphere.** The renderer already knows
// exactly what it drew — but it knows it one frame late and on the device (`renderer.md`, "The
// renderer never reads a buffer back inside a frame"), and a tier that lags the camera by a frame
// pops on every cut. So this is a fresh, conservative test of six planes against one sphere per
// instance, which is a handful of compares and is the same test the cull pass runs per *pair*. It
// is conservative in the same direction the cull pass is: the sphere carries the instance's bounds
// padding, so a limb that swings out of the rest-pose sphere is still "on screen" here, and an
// instance the test keeps is at worst animated too finely.
//
// `views` are the frame's, whose origin is the camera's eye (ADR-0053), and `positions` are world
// positions, so each is taken into that frame — `relative(position, eye)`, in f64 and then a float
// the size of its distance from the eye — before the test. `eye` is the camera's position.
inline void view_importance(const renderer::ViewSet& views, WorldPos eye,
                            std::span<const WorldPos> positions, std::span<const f32> radii,
                            std::span<f32> out) {
  const f32 best_weight = max_view_weight(views);
  Frustum frusta[renderer::k_max_views];
  f32 weights[renderer::k_max_views];
  const u32 view_count = views.size();
  for (u32 v = 0; v < view_count; ++v) {
    frusta[v] = frustum_from_view_proj(views[v].view_proj);
    weights[v] = view_weight(views[v]);
  }
  for (u32 i = 0; i < positions.size() && i < out.size(); ++i) {
    const f32 radius = i < radii.size() ? radii[i] : 0.0f;
    f32 seen = 0.0f;
    for (u32 v = 0; v < view_count; ++v) {
      if (weights[v] <= seen) continue;  // a lesser view cannot improve the answer
      if (frustum_contains_sphere(frusta[v], relative(positions[i], eye), radius))
        seen = weights[v];
    }
    out[i] = seen > 0.0f ? seen / best_weight : k_offscreen_importance;
  }
}

// The capability's bands, stretched by `scale`. `--anim-lod-scale 2` doubles every boundary, so
// everything is one band nearer than it was and the crowd animates finer; `0.5` halves them. It
// multiplies the boundaries rather than dividing the scores because the boundaries are what a
// game tunes and what `animation.lod.near/mid/far` already name, and because a scale of zero would
// otherwise be a division by zero rather than "everything is tier 3".
inline sim::TierParams scaled_tier_params(const sim::TierParams& base, f32 scale) noexcept {
  sim::TierParams out = base;
  const f32 factor = scale > 0.0f ? scale : 0.0f;
  for (u32 t = 0; t < sim::k_max_tiers; ++t)
    out.boundaries[t] = base.boundaries[t] * factor;
  return out;
}

}  // namespace engine::view
