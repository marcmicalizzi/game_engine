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

// ---- HDR output (E39; gfx/display.h says what and why; renderer.md, "HDR output") ------------
//
// The same encode for a display past [0, 1]: the display tone curve in units of paper white with
// its ceiling at the display's peak, the nits, then PQ into BT.2020 (dithered by one 10-bit PQ
// code) or scRGB's linear light (1.0 at 80 nits, no dither). display.h mirrors every function.

static const uint k_display_sdr = 0;   // gfx::DisplayEncoding::Sdr
static const uint k_display_pq = 1;    // gfx::DisplayEncoding::Pq
static const uint k_display_scrgb = 2; // gfx::DisplayEncoding::ScRgb
static const uint k_display_linear = 3; // gfx::DisplayEncoding::Linear: the exposed radiance

// What the frame's output is: the encoding, the target's steps (the dither's, 0 for none), paper
// white and the peak in nits. `ResolveParams`' display fields, gathered by the caller.
struct DisplayTarget {
  uint encoding;
  uint steps;
  float paper_white_nits;
  float peak_nits;
};

// display.h's `display_tone`: at or below `knee` the value, above it a roll-off towards
// `ceiling` with the line's slope where they meet. With `ceiling` 1 it is sky.slang's `sky_tone1`.
float display_tone1(float x, float knee, float ceiling) {
  if (x <= knee || knee >= ceiling) return x;
  return knee + (ceiling - knee) * (1.0 - exp(-(x - knee) / (ceiling - knee)));
}

// SMPTE ST 2084: nits to PQ's encoded value. display.h's `pq_encode`.
float3 pq_encode(float3 nits) {
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 4096.0 * 128.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 4096.0 * 32.0;
  const float c3 = 2392.0 / 4096.0 * 32.0;
  const float3 p = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// Linear Rec. 709 to linear BT.2020 (display.h's `k_bt709_to_bt2020`, rows).
float3 bt709_to_bt2020(float3 c) {
  return float3(dot(float3(0.6274038959, 0.3292830384, 0.0433130657), c),
                dot(float3(0.0690972894, 0.9195403951, 0.0113623156), c),
                dot(float3(0.0163914389, 0.0880133079, 0.8955952532), c));
}

// **The one output encode, with the display it is for.** `exposed` is radiance times the frame's
// exposure, *before* any shoulder; `knee` where the curve starts to roll off (the sky's shoulder,
// or 1 for a scene with no sky). SDR is exactly what the three-argument form and `sky_tone` drew;
// HDR10 and scRGB tone towards the peak instead of the SDR white (display.h says why).
//
// **The UI's seam** (04 §4.7; renderer.md, "HDR output"): an interface composited over the picture
// belongs between the curve and the encode — `nits` below, in the output's own primaries, at paper
// white — so this function would split there into a tone pass and an encode pass the frame ends
// on. Nothing composites yet, and nothing is built for it.
float3 display_output(float3 exposed, float knee, uint2 pixel, DisplayTarget target) {
  // The linear picture (display.h's `DisplayEncoding::Linear`): the radiance as a half holds it,
  // no curve, no dither — what an EXR of the scene's light is.
  if (target.encoding == k_display_linear) return min(max(exposed, 0.0), 65504.0);
  if (target.encoding == k_display_sdr) {
    const float3 toned = float3(display_tone1(exposed.x, knee, 1.0),
                                display_tone1(exposed.y, knee, 1.0),
                                display_tone1(exposed.z, knee, 1.0));
    return display_output(toned, pixel, target.steps);
  }
  const float ceiling = max(target.peak_nits / max(target.paper_white_nits, 1e-6), 1.0);
  const float3 e = max(exposed, 0.0);
  const float3 nits = float3(display_tone1(e.x, knee, ceiling), display_tone1(e.y, knee, ceiling),
                             display_tone1(e.z, knee, ceiling)) *
                      target.paper_white_nits;
  if (target.encoding == k_display_scrgb) return nits / 80.0;
  return display_dithered(pq_encode(bt709_to_bt2020(nits)), pixel, target.steps);
}

// The ramp test pattern's exposed radiance at `pixel` of a view `size` (display.h's
// `display_ramp`, which says what its three bands are): drawn through `display_output` with a
// knee of 1 by the resolve's ramp mode, in place of the scene.
float display_ramp(uint2 pixel, uint2 size) {
  const float u = size.x > 1 ? float(pixel.x) / float(size.x - 1) : 0.0;
  if (2 * pixel.y < size.y) return pow(u, 2.2);
  if (4 * pixel.y < 3 * size.y) return pow(u * 0.125, 2.2);
  return exp2(4.0 * u);
}

#endif  // ENGINE_DISPLAY_SLANG
