#pragma once

// sky capability (ADR-0027, ADR-0048; docs/subsystems/sky.md): the **model** of an Earth sky —
// where the sun and the moon stand on a calendar at a latitude (a low-precision ephemeris), what
// the air is made of (Rayleigh, a turbidity's aerosol, ozone), and which stars there are (a
// procedural table with the sky's magnitude counts). It draws nothing: the renderer draws a sky
// from what a sky provider says, and this capability registers the provider "earth"
// (`scene_gen::SkyProviderDesc`) that says it. Removed from the build, the renderer still draws
// every scene that names no sky, and refuses one that does with the registry's sentence.
//
// ADR-0027 checklist:
//   [x] one module in the layer it belongs to: domain, beside the terrain's generator; pure f64
//       arithmetic on the CPU, no GPU, no ECS
//   [x] OPTIONAL: ENGINE_WITH_SKY, off in the minimal build
//   [x] registration point: the sky provider "earth" in scene_gen's registry (sky_provider.cpp)
//   [ ] schema types      none of its own: the scene's `engine.scene.Sky` is the entry it reads
//   [ ] tick scheduler    none: the sky is a closed function of game time, evaluated per frame
//   [ ] render-graph pass none: drawing is the renderer's
//   [ ] protocol methods  none yet
//   [x] tunables          none: every number is the scene's (engine.scene.Sky) or a constant with a
//                         source (below)
//   [x] LOD policy        `star_magnitude_limit`: the table stops where a pixel stops showing stars
//   [x] determinism       `k_determinism` = derived
//   [x] zero cost unused  no linked code, and a scene with no sky never makes a provider
//   [x] tests, size table tests/sky_tests.cpp, tests/size_table.cpp
//   [ ] bench             no hot path: one ephemeris a frame, one table a scene

#include <core/base/types.h>

namespace engine::sky {

// Determinism stance (ADR-0010): **derived**. Every output is a closed function of the scene's sky
// entry and a game time, in f64, with nothing carried from one call to the next; nothing it says is
// read back into gameplay today. A game that wants the sun's height for heat reads the same
// function at the tick's time and gets the same answer on every machine the tree builds for
// (floating-point contraction is off, ADR-0035).
enum class Determinism : u8 { hashed, derived };
inline constexpr Determinism k_determinism = Determinism::derived;

// The LOD policy: stars down to this visual magnitude, about 8,500 of them — the naked eye's limit
// on the darkest night. A fainter star is under a display's step even on a moonless night at the
// exposure rule's darkest, so the table ends there; the sky has one observer and no tiers.
inline constexpr f64 k_star_magnitude_limit = 6.5;

}  // namespace engine::sky
