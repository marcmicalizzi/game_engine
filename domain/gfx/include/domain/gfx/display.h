#pragma once

// **The picture's output encode** (shaders/display.slang; docs/subsystems/gfx.md, "The output
// encode"; ADR-0052): where linear radiance, exposed and through the display's shoulder, becomes
// the colour target's code values. It happens once, in one function every writer of the picture
// calls — the resolve's shaded pixels and its sky, the reference path tracer's tonemap — and it is
// the display transfer curve (a 1/2.2 power) followed by a **dither**: triangular noise of one code
// step's amplitude added in the encoded domain before the store, so that a gradient slower than a
// code a pixel is drawn as a fine grain of the two codes around it instead of a band of one.
//
// The noise is a function of the pixel's place in its view and of nothing else — no frame index,
// no binding, not the view's offset in the target — so the same picture twice is the same bytes,
// and every equality the engine holds between rasterizers, between hosts, between layouts (a
// surround's centre view and a single view of that monitor) and between occlusion on and off holds
// unchanged: two pictures that agree before the encode agree after it. This header is the encode's
// CPU side: the code steps a target has, and the noise exactly as the shader computes it (integer
// arithmetic to a uniform, then one `sqrt` a device may round in its last bit), which the banding
// test and a 10-bit capture's reduction to 8 bits use.
//
// **The target's steps.** A UNORM target of b bits a channel holds 2^b - 1 steps, and one step is
// what the noise's amplitude is: 1/255 at 8 bits, 1/1023 at 10. A float target holds no steps and
// is never dithered; nor is an `_Srgb` target, whose store applies a curve of its own after the
// shader — the engine draws its picture into none (the swapchain chooser takes a UNORM format
// wherever a surface offers one), and dithering it rightly would mean scaling the noise through
// that curve's slope, so it gets no noise rather than a wrongly scaled one.

#include <core/base/types.h>
#include <domain/gfx/rhi.h>

#include <cmath>

namespace engine::gfx {

// Code steps a channel of a picture target has: 255 for 8-bit UNORM, 1023 for 10-bit, 0 for a
// float target, an `_Srgb` one, or anything that is not a colour target.
inline u32 display_steps(Format format) noexcept {
  switch (format) {
    case Format::R8G8B8A8Unorm:
    case Format::B8G8R8A8Unorm: return 255;
    case Format::A2R10G10B10Unorm:
    case Format::A2B10G10R10Unorm: return 1023;
    default: return 0;
  }
}

// Bits a colour channel of `format` holds: 8, 10, 16 or 32; 0 for a format that is no colour
// target. What a summary reports beside the format's name.
inline u32 display_bits(Format format) noexcept {
  switch (format) {
    case Format::R8G8B8A8Unorm:
    case Format::R8G8B8A8Srgb:
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb: return 8;
    case Format::A2R10G10B10Unorm:
    case Format::A2B10G10R10Unorm: return 10;
    case Format::R16G16B16A16Sfloat: return 16;
    case Format::R32G32B32A32Sfloat: return 32;
    default: return 0;
  }
}

// The format's name as a summary spells it ("A2B10G10R10Unorm"); "other" for one with no name here.
const char* format_name(Format format) noexcept;

// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare", SIGGRAPH 2014), `frac(52.9829189 * frac(0.06711056 x + 0.00583715 y))`: a uniform value
// in [0, 1) whose neighbours in any 3 x 3 window are spread over the whole range, so the noise has
// little energy at the low frequencies a viewer's eye averages over — measured, it leaves half the
// 8 x 8 residual white noise of the same strength does
// (docs/experiments/sky-banding-2026-10-04.md).
//
// **In fixed point, not float.** Each `frac` is a cliff: a value a last bit either side of an
// integer comes out near 0 or near 1, so the float form agrees with itself only where every step is
// rounded alike — and a GPU fuses and rounds as it likes (the first measurement found six pixels in
// 230,400 where the device's noise was the other end of the range from the CPU's). The same
// function as 32-bit fractions — the two gradients' constants times 2^32, the outer one times 2^16
// — is integer arithmetic, exact on every device, and the uniform is the top 24 bits of the result.
inline constexpr u32 k_display_ign_x = 288237660u;    // round(0.06711056 * 2^32)
inline constexpr u32 k_display_ign_y = 25070368u;     // round(0.00583715 * 2^32)
inline constexpr u64 k_display_ign_scale = 3472289u;  // round(52.9829189 * 2^16)
inline f32 display_ign(u32 x, u32 y) noexcept {
  const u32 inner = x * k_display_ign_x + y * k_display_ign_y;  // frac(...) * 2^32, mod 2^32
  const u32 outer = static_cast<u32>((static_cast<u64>(inner) * k_display_ign_scale) >> 16);
  return static_cast<f32>(outer >> 8) * (1.0f / 16777216.0f);
}

// The noise at pixel (x, y), in codes: the interleaved gradient noise through the inverse of the
// triangular distribution's cumulative function, which makes it triangular on [-1, 1) — the
// distribution whose rounding error has the same variance whatever the signal, so the grain does
// not come and go with the gradient — while keeping its spatial spread. display.slang's
// `display_dither`; its `sqrt` is the one step a device may round differently in the last bit.
inline f32 display_dither(u32 x, u32 y) noexcept {
  const f32 u = display_ign(x, y);
  return u < 0.5f ? std::sqrt(2.0f * u) - 1.0f : 1.0f - std::sqrt(2.0f * (1.0f - u));
}

// An encoded value in [0, 1] with the noise of pixel (x, y) at a target of `steps` code steps:
// what the shader hands the store, and the store rounds. The amplitude is one step, narrowed within
// one step of either end so that nothing is clipped — black stays black and white white, and the
// mean of every pixel stays its value. `steps` 0 is no dither. display.slang's
// `display_dithered`.
inline f32 display_dithered(f32 encoded, u32 x, u32 y, u32 steps) noexcept {
  if (steps == 0) return encoded;
  const f32 v = encoded < 0.0f ? 0.0f : (encoded > 1.0f ? 1.0f : encoded);
  const f32 s = static_cast<f32>(steps);
  f32 room = v * s;
  if ((1.0f - v) * s < room) room = (1.0f - v) * s;
  if (room > 1.0f) room = 1.0f;
  return v + display_dither(x, y) * room / s;
}

}  // namespace engine::gfx
