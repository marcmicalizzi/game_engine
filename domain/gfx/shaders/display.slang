// The picture's output encode (gfx/display.h, which mirrors it on the CPU; docs/subsystems/gfx.md,
// "The output encode"; ADR-0052). No bindings and no entry points, like brdf.slang and sky.slang:
// every writer of the picture includes it — the resolve (its shaded pixels and its sky) and the
// reference path tracer's tonemap — so linear radiance becomes the target's code values in one
// function and is quantized once, by the target's own store.
//
// **The encode** is the display's transfer curve, a 1/2.2 power, on radiance already exposed and
// through the sky's shoulder. **The dither** follows it: triangular noise of one code step of the
// target (`steps` = 2^bits - 1: 255 at 8 bits, 1023 at 10) added in the encoded domain, so that a
// gradient slower than a code a pixel becomes a fine grain of the two codes round it instead of a
// band of one — measured on the erg's sky at dusk, dawn and under the moon, an 8 x 8 average of the
// dithered picture is within 0.03-0.04 of a code of the float under it, against up to 0.19 for the
// rounded one (docs/experiments/sky-banding-2026-10-04.md). The noise is a function of the pixel's
// place in its view and of nothing else, so the same picture twice is the same bytes and two
// pictures that agree before the encode agree after it. `steps` 0 is no dither — a float target, a data view, a caller
// that asked for none — and returns exactly what the encode did before it existed.

#ifndef ENGINE_DISPLAY_SLANG
#define ENGINE_DISPLAY_SLANG

// The display transfer: linear to the encoded value a UNORM target stores. Not clamped: the store
// clamps, and a float target keeps what is over 1.
float3 display_encode(float3 linear_color) { return pow(max(linear_color, 0.0), 1.0 / 2.2); }

// Interleaved gradient noise (Jimenez 2014), `frac(52.9829189 * frac(0.06711056 x + 0.00583715 y))`,
// uniform on [0, 1): spread over the whole range in any 3 x 3 window, so it carries little of the
// low frequencies an eye averages. In 32-bit fixed point (display.h says why: a float `frac` is a
// cliff a device's rounding falls off), so this is display.h's `display_ign` to the bit.
float display_ign(uint2 pixel) {
  const uint inner = pixel.x * 288237660u + pixel.y * 25070368u;  // frac(...) * 2^32
  const uint outer = uint((uint64_t(inner) * uint64_t(3472289u)) >> 16);  // 52.9829189, 16.16
  return float(outer >> 8) * (1.0 / 16777216.0);
}

// The noise in codes: the uniform through the inverse of the triangular distribution's cumulative
// function, triangular on [-1, 1) — the distribution whose rounding error has the same variance
// whatever the signal — with the pattern's spread kept. display.h's `display_dither`.
float display_dither(uint2 pixel) {
  const float u = display_ign(pixel);
  return u < 0.5 ? sqrt(2.0 * u) - 1.0 : 1.0 - sqrt(2.0 * (1.0 - u));
}

// An encoded value with the noise of `pixel` at a target of `steps` steps: one step of amplitude,
// narrowed within a step of either end so nothing is clipped — black stays black, white white, and
// every pixel's mean stays its value. display.h's `display_dithered`.
float3 display_dithered(float3 encoded, uint2 pixel, uint steps) {
  if (steps == 0) return encoded;
  const float3 v = saturate(encoded);
  const float s = float(steps);
  const float3 room = min(min(v * s, (1.0 - v) * s), float3(1.0));
  precise float3 dithered = v + display_dither(pixel) * room / s;
  return dithered;
}

// **The one output encode**: linear radiance, exposed and toned, to what the target's store
// rounds. The resolve's shaded pixels and its sky, and the reference's tonemap, call nothing else.
float3 display_output(float3 linear_color, uint2 pixel, uint steps) {
  return display_dithered(display_encode(linear_color), pixel, steps);
}

#endif  // ENGINE_DISPLAY_SLANG
