#pragma once

// **A texture's levels resident by an exact bound** (R172 stage 1; ADR-0065; docs/subsystems/
// texture.md, "Levels resident by what the frame samples"). What `GpuScene::texture_bound_use`
// reports: how many of the load's built textures the bound manages, what they would hold whole and
// what they hold, and what the promotions that brought levels in off the frame cost. The state
// itself is the renderer's own (`src/texture_bound_state.h`, `gpu_scene_texture_bound.cpp`).

#include <core/base/types.h>

namespace engine::renderer {

struct TextureBoundUse {
  u32 managed = 0;         // the load's built textures the bound holds by level
  u64 whole_bytes = 0;     // what those would take uploaded whole (their chains and variance)
  u64 resident_bytes = 0;  // what they take now
  u64 peak_bytes = 0;      // the most they took at once, a leaving image included
  u64 promotions = 0;      // images swapped in for a finer top level
  u64 promoted_bytes = 0;  // the device bytes of those images
  u64 settles = 0;         // frames that brought every wanted level in before drawing (a cut)
  u64 evaluations = 0;     // times the bound was worked out (an observer moved far enough)
  // Frames on which a texture was wanted finer at the observer's own place than it was resident:
  // the lead was outrun and those pixels read a coarser level than the whole chain gives.
  u64 late = 0;
  f64 worst_admit_ms = 0.0;    // the most one frame's admissions took on the recording thread
  f64 worst_settle_ms = 0.0;   // the most one settle took (reads included)
  f64 last_evaluate_ms = 0.0;  // the last evaluation's cost
};

}  // namespace engine::renderer
