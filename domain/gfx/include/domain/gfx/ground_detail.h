#pragma once

// The ground's detail (shaders/ground_detail.slang; docs/subsystems/gfx.md, "The ground's detail";
// docs/subsystems/renderer.md, "The sand close up"): wind ripples and grain on a terrain's sand,
// **a function and not an image**. It is evaluated per pixel from the surface point's world (x, z),
// the scene's seed and the wind, with no texture coordinates, no tiles and nothing that meets at an
// edge, so it has no seam anywhere a function of position has none — across a cluster, an LOD cut,
// a ring of the terrain or a chunk of one — and it does not repeat.
//
// **The ripples are phasor noise** (Tricard, Efremov, Zanni, Neyret, Martínez, Lefebvre,
// "Procedural Phasor Noise", SIGGRAPH 2019): a sum of complex Gabor kernels on a jittered lattice,
// each a sinusoid oriented along the wind (within `spread` of it) under a smooth bell, whose
// **argument** is the pattern's phase. Chosen over a sum of real Gabor kernels because the sum's
// contrast wanders wherever kernels cancel, and its profile can only be a sinusoid; the phase is a
// field of constant contrast that any periodic profile can be laid on — here an asymmetric one, a
// long gentle windward slope and a short steep lee — and its **singularities**, where the kernels
// cancel, are exactly the defects real ripples have: a crest that ends, two crests joining one. How
// often that happens is the kernels' size in wavelengths (a narrower spectrum is longer, straighter
// crests), which is what `GroundDetailDesc::ripple_defects` sets. The amplitude tapers where the
// sum is small, so a defect is a crest fading out rather than a point where every phase meets.
//
// **The filter** makes it look natural at a distance: the ripples fade by how many of their
// wavelengths one pixel spans along the wind, measured from the reconstruction's own derivatives
// per screen axis (a grazing footprint is long and thin, and only its extent across the crests
// aliases), in two stages — the profile's harmonics first, the asymmetric profile easing to a
// plain sinusoid between eight pixels a wavelength and four, since its steep lee is a quarter of a
// wavelength wide; then the sinusoid, between four pixels and two (`ground_fade`) — and the slope
// variance each stage takes moves into the GGX roughness (`alpha^2 += sigma^2 - drawn sigma^2`),
// so distant sand is rougher, not a mirror. The grain fades the same way by its cells.
//
// The block below is what the resolve and the path tracer read through an address
// (`ResolveParams::ground_detail`, `PathTraceParams::ground_detail`); a material draws the detail
// when it has `k_material_ground_detail` and the address is not zero. `ground_detail_block` turns a
// scene's numbers into it, and `domain/gfx/tests/ground_detail_reference.h` evaluates the same
// function in double precision on the CPU.

#include <core/base/types.h>
#include <core/math/math.h>

#include <algorithm>
#include <cmath>

namespace engine::gfx {

// GroundDetailParams::flags.
inline constexpr u32 k_ground_ripples = 1u;  // the phasor ripples perturb the shading normal
inline constexpr u32 k_ground_grain = 2u;    // the grain varies the albedo and the roughness
// The ripples fade on ground that faces away from the wind (`lee_tan_start`/`end`): exposure.
inline constexpr u32 k_ground_exposure = 4u;
// The grain is gradient noise in `grain_octaves` octaves (and may lean the normal, `grain_normal`)
// rather than the first pass's two octaves of value noise.
inline constexpr u32 k_ground_gradient_grain = 8u;
// Grainflow streaks down the fall line where the ground falls away from the wind near the angle of
// repose (`streak_*`).
inline constexpr u32 k_ground_streaks = 16u;

// Streak kernels per lattice cell of `streak_length`: three cover about four fifths of a slip face
// at the default width, so tongues touch and cross but leave sand between them.
inline constexpr u32 k_ground_streak_impulses = 3;

// The most octaves the grain takes: a 2 cm grain down to 0.2 mm is seven.
inline constexpr u32 k_ground_max_grain_octaves = 8;
// Gradient noise as the grain draws it (a corner's gradient five bits a component in [-1, 1], the
// quintic fade) has a value of root-mean-square 0.182 and a gradient of 0.743 per cell over
// (x, z), measured over the mirror (ground_detail_tests.cpp, "the grain reads as sand"). The
// grain's albedo and roughness channels are scaled to `k_ground_grain_rms` over all its octaves,
// which is what the first pass's two octaves of value noise had, so `grain_albedo` means what it
// meant; its height to a slope of `grain_normal` root-mean-square.
inline constexpr f32 k_ground_noise_rms = 0.182f;
inline constexpr f32 k_ground_noise_slope_rms = 0.743f;
inline constexpr f32 k_ground_grain_rms = 0.28f;

// Kernels per lattice cell, mirrored in the shader. A point sums the 3 x 3 cells round it, eighteen
// kernels, of which about a third reach it (a kernel's disc is pi R^2 of the 9 R^2 searched): with
// one a cell the sum leans on the lattice, a cell's single kernel being most of the field at its
// centre, and every further one is six more kernels a pixel.
inline constexpr u32 k_ground_impulses = 2;

// The taper's floor, as a share of the sum's root-mean-square magnitude: `m = |S|^2 / (|S|^2 +
// (k |S|_rms)^2)`. Where the kernels cancel, `m` takes the ripple down with them, so a defect is a
// crest fading out; above about twice `k`, the ripple stands at nearly its full height.
inline constexpr f32 k_ground_taper = 0.35f;

// The share of the profile's slope variance the phasor pattern has, measured over the pattern
// itself (domain/gfx/tests/ground_detail_tests.cpp, the first case: E|grad h|^2 over 160,000
// points is 1.00 of this at the default numbers, 1.00 at the sinusoid the filter eases to, 1.05
// with no defects and 1.08 with the most): the taper lowers the ripples near every defect, and the
// phase's gradient is longer than the wavelength says near them. The roughness transfer moves this
// much, so the rule moves the variance the pattern has, not the variance a perfect ripple would.
inline constexpr f32 k_ground_slope_share = 0.65f;

// Mirrors GroundDetail in shaders/ground_detail.slang. 128 bytes: the first pass's 80, and the
// second's grain, streaks and three words spare.
struct GroundDetailParams {
  // xy: the direction the sand moves, (x, z), unit: the ripples' phase grows along it, so a crest's
  // gentle side faces up the wind and its lee down it. z: sin of the kernels' orientation spread.
  Vec4 wind{1.0f, 0.0f, 0.0f, 0.0f};
  f32 wavelength = 0.12f;  // metres, crest to crest
  f32 amplitude = 0.004f;  // metres: half the crest-to-trough height
  f32 asymmetry = 0.75f;   // the windward share of a wavelength, (0, 1)
  f32 cell = 0.5f;  // metres: a kernel's radius, and the side of the lattice cells they sit in
  // `(k_ground_taper |S|_rms)^2`, the taper's floor, with `|S|_rms^2 = k_ground_impulses pi / 7`
  // for the kernel `(1 - r^2/R^2)^3` at `k_ground_impulses` a cell of side R.
  f32 magnitude_floor = 0.11f;
  // The ripples' slope variance at full height, `E|grad h|^2`, is `slope_scale (1/a + 1/(1 - a))`
  // at asymmetry a (`ground_slope_scale`): what the filter moves into the roughness as the profile
  // eases to a sinusoid and then fades.
  f32 slope_scale = 0.0f;
  // The ripples' fade by the ground's slope, as the shading normal's y: all of them at or above
  // `slope_cos_start`, none at or below `slope_cos_end` (a slip face at the angle of repose is
  // avalanched smooth).
  f32 slope_cos_start = 1.0f;
  f32 slope_cos_end = 0.0f;
  f32 grain_size = 0.02f;      // metres: the coarse octave's cell; the fine one's is a quarter
  f32 grain_albedo = 0.0f;     // the albedo's variation, a share of it either way
  f32 grain_roughness = 0.0f;  // the perceptual roughness's, either way
  u32 seed = 0;                // the scene's: what every kernel and lattice value is drawn from
  u32 flags = 0;               // k_ground_ripples | k_ground_grain | k_ground_exposure
  // The exposure (k_ground_exposure): the ground's fall along the wind, `dot(n.xz, wind) / n.y`
  // (the tangent of its slope down the wind: negative climbing into it, positive falling away),
  // at which the ripples start to go, and at and past which there are none.
  f32 lee_tan_start = 0.0f;
  f32 lee_tan_end = 0.0f;
  // The gradient grain (k_ground_gradient_grain): octaves from `grain_size` halving down to the
  // scene's finest, and the root-mean-square slope its height leans the normal by.
  u32 grain_octaves = 0;
  f32 grain_normal = 0.0f;
  // The streaks (k_ground_streaks): the ground's fall along the wind (as `lee_tan_*`) at which they
  // start and at which they are whole; a tongue's width and length, metres; and what the sum of the
  // tongues, scaled to unit root-mean-square here, moves: the albedo (a share), the roughness, and
  // the normal (`streak_slope`, the sum's gradient to a root-mean-square slope of `streak_normal`).
  f32 streak_tan_start = 0.0f;
  f32 streak_tan_full = 0.0f;
  f32 streak_width = 0.4f;
  f32 streak_length = 2.0f;
  f32 streak_albedo = 0.0f;
  f32 streak_roughness = 0.0f;
  f32 streak_slope = 0.0f;
  f32 streak_normal = 0.0f;
  u32 pad3 = 0;
  u32 pad4 = 0;
  u32 pad5 = 0;
};
static_assert(sizeof(GroundDetailParams) == 128);
static_assert(sizeof(GroundDetailParams) % 16 == 0, "the block is read as float4 rows on the GPU");

// The scene's numbers (the renderer's `engine.scene.TerrainDetail`): what a scene says, in metres
// and degrees, before the mechanism's own constants are derived from them.
struct GroundDetailDesc {
  f32 ripple_wavelength = 0.12f;  // metres crest to crest
  f32 ripple_height = 0.008f;     // metres crest to trough; 0 draws no ripples
  f32 ripple_asymmetry = 0.75f;   // the windward share of a wavelength
  // How often crests end and join, [0, 1]: it sets the kernels' radius from six wavelengths at 0
  // (long straight crests) to one and a half at 1, and their spread about the wind from 2 to 20
  // degrees.
  f32 ripple_defects = 0.35f;
  f32 slope_start_deg = 22.0f;  // the ground's slope at which the ripples start to fade
  f32 slope_end_deg = 30.0f;    // and past which there are none
  // The ripples' exposure to the wind: on ground falling away from it, they start to go at a lee
  // slope of `lee_start_deg` along the wind and are gone at `lee_end_deg`. Both 0 (the default):
  // no exposure, the first pass's ripples, which fade by steepness alone.
  f32 lee_start_deg = 0.0f;
  f32 lee_end_deg = 0.0f;
  f32 grain_size = 0.02f;
  f32 grain_albedo = 0.08f;
  f32 grain_roughness = 0.05f;
  // The grain's finest octave, metres: gradient noise in octaves from `grain_size` halving down to
  // it. 0 (the default) is the first pass's two octaves of value noise, `grain_size` and a quarter.
  f32 grain_finest = 0.0f;
  // The root-mean-square slope the grain's height leans the shading normal by; 0 none.
  f32 grain_normal = 0.0f;
  // Grainflow streaks on a slip face: on ground falling away from the wind they start at a lee
  // slope of `streak_start_deg` along it and are whole at `streak_full_deg`; both 0 (the default)
  // is none. Tongues `streak_width` across and `streak_length` long down the fall line, moving the
  // albedo by `streak_albedo` (a share, root-mean-square), the roughness by `streak_roughness` and
  // the normal by a root-mean-square slope of `streak_normal`.
  f32 streak_start_deg = 0.0f;
  f32 streak_full_deg = 0.0f;
  f32 streak_width = 0.4f;
  f32 streak_length = 2.0f;
  f32 streak_albedo = 0.06f;
  f32 streak_roughness = 0.04f;
  f32 streak_normal = 0.012f;
};

// The gradient grain's octaves for a coarsest cell and a finest: `grain_size` halving until the
// next would be under `grain_finest` (a thousandth of slack, so 2 cm to 1.25 mm is five), at most
// k_ground_max_grain_octaves; 0 when there is no finest, the first pass's grain.
inline u32 ground_grain_octaves(f32 grain_size, f32 grain_finest) noexcept {
  if (!(grain_finest > 0.0f) || !(grain_size > 0.0f)) return 0;
  u32 n = 1;
  f32 size = grain_size;
  while (n < k_ground_max_grain_octaves && size * 0.5f >= grain_finest * 0.999f) {
    size *= 0.5f;
    ++n;
  }
  return n;
}

// The kernels' radius and spread for a defect density, and the ripples' slope variance: the
// arithmetic the block carries so the shader does not repeat it per pixel.
inline f32 ground_cell(const GroundDetailDesc& desc) noexcept {
  const f32 d = std::clamp(desc.ripple_defects, 0.0f, 1.0f);
  return desc.ripple_wavelength * (6.0f + (1.5f - 6.0f) * d);
}
inline f32 ground_spread_rad(const GroundDetailDesc& desc) noexcept {
  const f32 d = std::clamp(desc.ripple_defects, 0.0f, 1.0f);
  return radians(2.0f + (20.0f - 2.0f) * d);
}
// The profile's `E|dh/ds|^2` along the wind for a ripple of half-height `a` and wavelength `l`:
// the warped cosine rises over the windward share `w` and falls over the rest, so its slope is
// `a 2 pi sin(2 pi u) / (2 w l)` on the one and `/ (2 (1 - w) l)` on the other, and sin^2 averages
// a half over each: `(pi a / l)^2 (1/w + 1/(1 - w)) / 2`. The pattern has `k_ground_slope_share`
// of it. This is the factor in front of `(1/w + 1/(1 - w))`, which the shader completes for the
// asymmetry it draws.
inline f32 ground_slope_scale(f32 half_height, f32 wavelength) noexcept {
  const f64 s =
      3.14159265358979323846 * static_cast<f64>(half_height) / static_cast<f64>(wavelength);
  return static_cast<f32>(s * s * 0.5 * static_cast<f64>(k_ground_slope_share));
}
inline f32 ground_slope_variance(const GroundDetailParams& d, f32 asymmetry) noexcept {
  return d.slope_scale * (1.0f / asymmetry + 1.0f / (1.0f - asymmetry));
}

// The block for a scene's numbers, the wind's direction over (x, z) — any length; normalized here,
// and +x when it is zero — and the scene's seed.
inline GroundDetailParams ground_detail_block(const GroundDetailDesc& desc, Vec2 wind,
                                              u32 seed) noexcept {
  GroundDetailParams out;
  const f32 length = std::sqrt(wind.x * wind.x + wind.y * wind.y);
  const Vec2 w = length > 1e-12f ? Vec2{wind.x / length, wind.y / length} : Vec2{1.0f, 0.0f};
  out.wind = Vec4{w.x, w.y, std::sin(ground_spread_rad(desc)), 0.0f};
  out.wavelength = desc.ripple_wavelength;
  out.amplitude = 0.5f * desc.ripple_height;
  out.asymmetry = std::clamp(desc.ripple_asymmetry, 0.05f, 0.95f);
  out.cell = ground_cell(desc);
  const f32 rms2 = static_cast<f32>(k_ground_impulses) * k_pi / 7.0f;
  out.magnitude_floor = k_ground_taper * k_ground_taper * rms2;
  out.slope_scale = ground_slope_scale(out.amplitude, out.wavelength);
  out.slope_cos_start = std::cos(radians(desc.slope_start_deg));
  out.slope_cos_end = std::cos(radians(desc.slope_end_deg));
  out.grain_size = desc.grain_size;
  out.grain_albedo = desc.grain_albedo;
  out.grain_roughness = desc.grain_roughness;
  out.seed = seed;
  out.flags = (desc.ripple_height > 0.0f ? k_ground_ripples : 0u) |
              (desc.grain_albedo > 0.0f || desc.grain_roughness > 0.0f ? k_ground_grain : 0u);
  out.grain_octaves = ground_grain_octaves(desc.grain_size, desc.grain_finest);
  if (out.grain_octaves > 0) {
    out.grain_normal = desc.grain_normal;
    if (desc.grain_albedo > 0.0f || desc.grain_roughness > 0.0f || desc.grain_normal > 0.0f)
      out.flags |= k_ground_grain | k_ground_gradient_grain;
  }
  if (desc.streak_full_deg > desc.streak_start_deg && desc.streak_width > 0.0f &&
      desc.streak_length > 0.0f) {
    out.flags |= k_ground_streaks;
    out.streak_tan_start = std::tan(radians(desc.streak_start_deg));
    out.streak_tan_full = std::tan(radians(desc.streak_full_deg));
    out.streak_width = desc.streak_width;
    out.streak_length = desc.streak_length;
    // The tongues' sum has `E S^2 = k / (3 L^2) pi a b / 7` for k kernels a cell of side L, each an
    // amplitude uniform in [-1, 1] under `(1 - r^2)^3` over an ellipse of half-axes a = L/2 and
    // b = W/2, and `E |grad S|^2 = k / (3 L^2) 0.6 pi a b (1/a^2 + 1/b^2)`; the block scales the
    // sum by the first's root and its gradient by the second's (ground_detail_tests.cpp measures
    // both).
    const f64 a = 0.5 * static_cast<f64>(desc.streak_length);
    const f64 b = 0.5 * static_cast<f64>(desc.streak_width);
    const f64 density =
        static_cast<f64>(k_ground_streak_impulses) /
        (3.0 * static_cast<f64>(desc.streak_length) * static_cast<f64>(desc.streak_length));
    const f64 pi = 3.14159265358979323846;
    const f64 rms = std::sqrt(density * pi * a * b / 7.0);
    const f64 slope_rms = std::sqrt(density * 0.6 * pi * a * b * (1.0 / (a * a) + 1.0 / (b * b)));
    out.streak_albedo = static_cast<f32>(static_cast<f64>(desc.streak_albedo) / rms);
    out.streak_roughness = static_cast<f32>(static_cast<f64>(desc.streak_roughness) / rms);
    out.streak_slope = static_cast<f32>(static_cast<f64>(desc.streak_normal) / slope_rms);
    out.streak_normal = desc.streak_normal;
  }
  if (desc.lee_end_deg > desc.lee_start_deg) {
    out.flags |= k_ground_exposure;
    out.lee_tan_start = std::tan(radians(desc.lee_start_deg));
    out.lee_tan_end = std::tan(radians(desc.lee_end_deg));
  }
  return out;
}

}  // namespace engine::gfx
