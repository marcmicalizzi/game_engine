#pragma once

// **The material resolve's pixel classes** (resolve_class.slang, resolve_classify.slang;
// docs/subsystems/renderer.md, "The resolve by pixel class"; roadmap R151). The classify pass
// writes each pixel's class as depth, `resolve_class_depth(class)`, and the resolve is drawn once
// per class at that depth with a depth-equal test. This header is the shader's constants on the
// CPU's side; the pass and its pipelines are the renderer's.

#include <core/base/types.h>

namespace engine::gfx {

// What a pixel is, as far as the resolve's cost goes: the kinds R148's census told apart
// (docs/experiments/resolve-cost-2026-11-11.md), and the wet ground, which is ground that needs
// the water module. Mirrored by resolve_class.slang, value for value.
enum class ResolveClass : u32 {
  Sky = 0,       // no surface
  Rigid = 1,     // a rigid, opaque mesh
  Ground = 2,    // a layered ground
  Masked = 3,    // an alpha-tested material
  Deformed = 4,  // a deformed instance's opaque parts
  Water = 5,     // a water material (only the water variant writes it)
  Wet = 6,       // ground within a shore's or a bank's reach (only the water variant writes it)
};
// Seven, class c at (c + 1) sixteenths: every class's depth is exact in a float and in the
// viewport's transform. Depth 0 is no class's — a fullscreen triangle under the default depth
// range, which is the one draw's, has it — and neither is the classify pass's clear, 1.0.
inline constexpr u32 k_resolve_classes = 7;
inline constexpr f32 k_resolve_class_depth_step = 0.0625f;
constexpr f32 resolve_class_depth(u32 resolve_class) noexcept {
  return static_cast<f32>(resolve_class + 1) * k_resolve_class_depth_step;
}
// The classes whose pixels may run the water module's code: a scene's other pixels can be drawn
// by the module that has none.
inline constexpr u32 k_resolve_class_water_mask =
    (1u << static_cast<u32>(ResolveClass::Water)) | (1u << static_cast<u32>(ResolveClass::Wet));
inline constexpr u32 k_resolve_class_all = (1u << k_resolve_classes) - 1u;

constexpr const char* resolve_class_name(u32 resolve_class) noexcept {
  constexpr const char* k_names[k_resolve_classes] = {"sky",      "rigid", "ground", "masked",
                                                      "deformed", "water", "wet"};
  return resolve_class < k_resolve_classes ? k_names[resolve_class] : "none";
}

}  // namespace engine::gfx
