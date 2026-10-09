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
#include <span>
#include <string>

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

// A surface's offers in one line, "B8G8R8A8Unorm srgb_nonlinear, A2B10G10R10Unorm hdr10_st2084,
// ...", a format or colour space with no name as its number: what a refusal names (E39).
std::string describe_surface_formats(std::span<const SurfaceFormat> formats);

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

// ---- HDR output (E39; docs/experiments/hdr-output-proposal.md, renderer.md "HDR output") ------
//
// **Built so the owner can measure, not decided.** Two HDR routes beside the SDR encode, chosen by
// the colour target the picture is drawn into: **HDR10** — linear Rec. 709 radiance to BT.2020
// primaries, in nits, through SMPTE ST 2084 (PQ), dithered by one 10-bit PQ code, into
// `A2B10G10R10Unorm` in the HDR10_ST2084 colour space — and **scRGB** — the same nits as linear
// Rec. 709 with 1.0 at 80 nits, into `R16G16B16A16Sfloat` in EXTENDED_SRGB_LINEAR, undithered.
// Which of them the engine keeps is the measurement's to say (no ADR until it has run). Both start
// from the same **display tone curve** (`display_tone`): the SDR picture's shoulder with its
// ceiling moved from the SDR white to the display's **peak**, in units of **paper white** — the
// luminance a diffuse white is shown at, where the SDR picture's white and (one day) the UI sit. So
// everything the SDR picture shows unrolled — below the sky's shoulder (0.6 of white), and for a
// scene with no sky everything below paper white — is shown at exactly the luminance the SDR
// picture shows it at paper white, and what the SDR picture rolls off into its last 40% is spread
// up to the peak instead. At a peak equal to paper white the curve is the SDR shoulder exactly.
// display.slang is the GPU side, `domain/gfx/tests/display_reference.h` the same in double.

// What the output encode writes: the SDR transfer (the 1/2.2 power) into a UNORM target, PQ, or
// scRGB's linear light. `ResolveParams::display_encoding`; the numbers are display.slang's.
enum class DisplayEncoding : u32 { Sdr = 0, Pq = 1, ScRgb = 2 };

// "sdr", "hdr10", "scrgb": what a summary says.
inline const char* display_encoding_name(DisplayEncoding encoding) noexcept {
  switch (encoding) {
    case DisplayEncoding::Pq: return "hdr10";
    case DisplayEncoding::ScRgb: return "scrgb";
    default: return "sdr";
  }
}

// What a presenting host asks its swapchain for (engine-view's `--present-format`): `Auto` takes
// 10 bits in the sRGB non-linear colour space where the surface offers them and 8 where it does
// not (`--present-bits auto`, ADR-0052), `Sdr8` 8 bits, `Sdr10` 10 bits where offered (8 where not,
// as `--present-bits 10` always did); `Hdr10` and `ScRgb` exactly the HDR format and colour space
// above, refused where the surface does not offer them.
enum class PresentFormat : u8 { Auto, Sdr8, Sdr10, Hdr10, ScRgb };

inline const char* present_format_name(PresentFormat format) noexcept {
  switch (format) {
    case PresentFormat::Sdr8: return "sdr8";
    case PresentFormat::Sdr10: return "sdr10";
    case PresentFormat::Hdr10: return "hdr10";
    case PresentFormat::ScRgb: return "scrgb";
    default: return "auto";
  }
}

// The encoding a present format draws with: PQ for HDR10, scRGB for scRGB, the SDR curve else.
inline DisplayEncoding present_format_encoding(PresentFormat format) noexcept {
  return format == PresentFormat::Hdr10   ? DisplayEncoding::Pq
         : format == PresentFormat::ScRgb ? DisplayEncoding::ScRgb
                                          : DisplayEncoding::Sdr;
}

// The colour target an HDR encoding draws into offscreen and asks a swapchain for: 10-bit packed
// for PQ, half float for scRGB; Undefined for SDR, whose target is the host's choice.
inline Format display_encoding_format(DisplayEncoding encoding) noexcept {
  return encoding == DisplayEncoding::Pq      ? Format::A2B10G10R10Unorm
         : encoding == DisplayEncoding::ScRgb ? Format::R16G16B16A16Sfloat
                                              : Format::Undefined;
}

// The defaults a display that reports nothing is drawn for, and the two fixed points of the
// encodings: scRGB's 1.0 and PQ's 1.0 (its code 1023).
inline constexpr f32 k_default_peak_nits = 1000.0f;
inline constexpr f32 k_default_paper_white_nits = 200.0f;
inline constexpr f32 k_scrgb_unit_nits = 80.0f;
inline constexpr f32 k_pq_max_nits = 10000.0f;

// SMPTE ST 2084 (PQ): the constants are the standard's rationals.
inline constexpr f32 k_pq_m1 = 2610.0f / 16384.0f;
inline constexpr f32 k_pq_m2 = 2523.0f / 4096.0f * 128.0f;
inline constexpr f32 k_pq_c1 = 3424.0f / 4096.0f;
inline constexpr f32 k_pq_c2 = 2413.0f / 4096.0f * 32.0f;
inline constexpr f32 k_pq_c3 = 2392.0f / 4096.0f * 32.0f;

// Nits (0..10,000) to PQ's encoded value in [0, 1]: display.slang's `pq_encode`.
inline f32 pq_encode(f32 nits) noexcept {
  const f32 y = nits <= 0.0f ? 0.0f : (nits >= k_pq_max_nits ? 1.0f : nits / k_pq_max_nits);
  const f32 p = std::pow(y, k_pq_m1);
  return std::pow((k_pq_c1 + k_pq_c2 * p) / (1.0f + k_pq_c3 * p), k_pq_m2);
}

// PQ's encoded value back to nits: what a test decodes a captured code with.
inline f32 pq_decode(f32 encoded) noexcept {
  const f32 e = encoded <= 0.0f ? 0.0f : (encoded >= 1.0f ? 1.0f : encoded);
  const f32 p = std::pow(e, 1.0f / k_pq_m2);
  const f32 num = p - k_pq_c1 > 0.0f ? p - k_pq_c1 : 0.0f;
  return k_pq_max_nits * std::pow(num / (k_pq_c2 - k_pq_c3 * p), 1.0f / k_pq_m1);
}

// Linear Rec. 709 to linear BT.2020, both with the D65 white (ITU-R BT.2087): rows of the matrix,
// each summing to 1 so white stays white, every entry positive so nothing goes negative and no
// channel exceeds the largest it was made from. display_reference.h derives it from the primaries
// in double and the CPU test holds these to that.
inline constexpr f32 k_bt709_to_bt2020[3][3] = {{0.6274038959f, 0.3292830384f, 0.0433130657f},
                                                {0.0690972894f, 0.9195403951f, 0.0113623156f},
                                                {0.0163914389f, 0.0880133079f, 0.8955952532f}};

// **The display tone curve**, one channel, in units of paper white: `x` exposed radiance (1.0 is
// a white Lambertian surface under the metered light), `knee` where the roll-off starts (the sky's
// shoulder, 0.6; 1 for a scene without a sky, whose SDR picture clips), `ceiling` what it rolls off
// towards — 1 for the SDR picture, the display's peak over paper white for HDR. At or below the
// knee the value is itself; above, it approaches the ceiling with the line's slope where they meet.
// With `ceiling` 1 this is `sky_tone1` (sky.slang) operation for operation, and with a knee at or
// above the ceiling it is the identity (the SDR picture's clip is the store's).
inline f32 display_tone(f32 x, f32 knee, f32 ceiling) noexcept {
  if (x <= knee || knee >= ceiling) return x;
  return knee + (ceiling - knee) * (1.0f - std::exp(-(x - knee) / (ceiling - knee)));
}

// Peak over paper white, never below 1: a display whose peak is under paper white is drawn with no
// headroom (its HDR picture is the SDR one at paper white), not with a ceiling below its white.
inline f32 display_headroom(f32 peak_nits, f32 paper_white_nits) noexcept {
  if (!(paper_white_nits > 0.0f)) return 1.0f;
  const f32 h = peak_nits / paper_white_nits;
  return h > 1.0f ? h : 1.0f;
}

// One pixel of the HDR encodes, as display.slang's `display_output` computes it with the dither
// off: `exposed` linear Rec. 709 radiance, `knee` as above. HDR10 gives the three PQ values in
// [0, 1] (BT.2020); scRGB the linear Rec. 709 values with 1.0 at 80 nits. What the renderer's clear
// colour and a test's expectation are computed with.
inline void display_encode_hdr(const f32 exposed[3], f32 knee, DisplayEncoding encoding,
                               f32 paper_white_nits, f32 peak_nits, f32 out[3]) noexcept {
  const f32 ceiling = display_headroom(peak_nits, paper_white_nits);
  f32 nits[3];
  for (u32 c = 0; c < 3; ++c)
    nits[c] = display_tone(exposed[c] > 0.0f ? exposed[c] : 0.0f, knee, ceiling) * paper_white_nits;
  if (encoding == DisplayEncoding::ScRgb) {
    for (u32 c = 0; c < 3; ++c)
      out[c] = nits[c] / k_scrgb_unit_nits;
    return;
  }
  for (u32 r = 0; r < 3; ++r) {
    const f32 n = k_bt709_to_bt2020[r][0] * nits[0] + k_bt709_to_bt2020[r][1] * nits[1] +
                  k_bt709_to_bt2020[r][2] * nits[2];
    out[r] = pq_encode(n);
  }
}

// **The HDR10 static metadata for a picture the curve has fitted to the display** (E39): the
// mastering primaries BT.2020's (what the PQ encode writes), its luminance range the display's own
// peak and black, MaxCLL the peak — the curve approaches it and never passes it — and MaxFALL paper
// white, where a frame's diffuse average sits. A display handed these has nothing left to tone-map.
inline HdrMetadata hdr10_metadata(f32 peak_nits, f32 min_nits, f32 paper_white_nits) noexcept {
  HdrMetadata m;
  m.max_luminance = peak_nits;
  m.min_luminance = min_nits;
  m.max_content_light_level = peak_nits;
  m.max_frame_average_light_level = paper_white_nits < peak_nits ? paper_white_nits : peak_nits;
  return m;
}

// The SDR picture's encoded value (the 1/2.2 power, display.slang's `display_encode`) back to the
// radiance it shows relative to its white: what an HDR picture is compared with.
inline f32 sdr_decode(f32 encoded) noexcept {
  return std::pow(encoded <= 0.0f ? 0.0f : encoded, 2.2f);
}

}  // namespace engine::gfx
