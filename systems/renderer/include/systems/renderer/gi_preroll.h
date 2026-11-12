#pragma once

// **The cache's pre-roll** (docs/subsystems/renderer.md, "Indirect light: the world-space cache",
// "Stage 5"; roadmap R69 stage 5). The world-space cache fills from the frames it is shown in, so
// the first picture after a load was the sky's harmonics and the next second was the light
// arriving; a camera cut, a teleport or fast travel is the same start somewhere else. A frame that
// starts cold therefore records the cache's sample pass and fold `renderer.gi.preroll` times more,
// against the frame's own visibility buffer and acceleration structures, before its resolve reads:
// each step is a frame of the cache's clock (its pixel offsets, its directions and its seeds turn),
// so sixteen steps are the samples of sixteen frames at one pose — the trickle's one pixel in nine
// has covered every pixel, the samples round the eye have covered the sphere sixteen times, and a
// bounce's hits have read cells that already hold the bounce before them.
//
// **Which frames start cold**: the cache's first (it is cleared there), and one whose eye is more
// than `renderer.gi.cut_m` from the frame before's. A walk moves a few centimetres a frame and a
// flight at 20 m/s a third of a metre; nothing continuous moves eight metres in a frame, and a
// frame that slow is a hitch already. A turn is not a cut: the cells round the eye are warm
// whatever it looks at (the samples round the eye, eye_shell.h and stage 4).
//
// Pure arithmetic, held by `gi cache: the pre-roll runs on the first frame and on a cut`.

#include <core/base/types.h>
#include <core/math/world.h>
#include <domain/gfx/gi_cache.h>
#include <systems/renderer/settings.h>

namespace engine::renderer {

// `ResolveParams::gi_flags` for a frame that runs the cache: the read, the mode, the levers of
// stages 2 to 6 and their numbers (gi_cache.h, `k_gi_*`), from the settings as resolved at load.
inline u32 gi_frame_flags(const ResolvedSettings& s) noexcept {
  return gfx::k_gi_read | (s.gi_mode == GiMode::Bounce ? gfx::k_gi_bounce : 0u) |
         (s.gi_face_blend ? gfx::k_gi_faces : 0u) | (s.gi_young_rays ? gfx::k_gi_young_rays : 0u) |
         (s.gi_hit_fallback ? gfx::k_gi_hit_fallback : 0u) | (s.gi_reveal ? gfx::k_gi_reveal : 0u) |
         (s.gi_show_level >= 0
              ? gfx::k_gi_show | (static_cast<u32>(s.gi_show_level) << gfx::k_gi_show_shift)
              : 0u) |
         gfx::gi_flag_numbers(s.gi_confident, s.gi_evict_age, s.gi_far_lift);
}

// True when the eye is more than `cut_m` metres from where the frame before had it. False while
// there is no frame before (the first frame is cold for its own reason) and for a `cut_m` of 0 or
// less, which turns the test off.
inline bool gi_eye_jumped(bool known, const WorldPos& last, const WorldPos& now,
                          f32 cut_m) noexcept {
  if (!known || !(cut_m > 0.0f)) return false;
  const f64 dx = now.x - last.x;
  const f64 dy = now.y - last.y;
  const f64 dz = now.z - last.z;
  const f64 reach = static_cast<f64>(cut_m);
  return dx * dx + dy * dy + dz * dz > reach * reach;
}

// The sample-and-fold pairs a frame records: its own, and `preroll` before it when it starts cold.
constexpr u32 gi_preroll_steps(bool cold, u32 preroll) noexcept { return 1 + (cold ? preroll : 0); }

}  // namespace engine::renderer
