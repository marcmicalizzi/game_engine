#pragma once

// The sky (shaders/sky.slang, shaders/sky_luts.slang; docs/subsystems/gfx.md, "The sky";
// docs/subsystems/renderer.md, "The sky"): a physical atmosphere of the kind Hillaire drew in "A
// Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020) — Rayleigh and
// Mie scattering and ozone absorption over a spherical planet, single scattering by ray marching
// and every higher order by his isotropic approximation, the Psi_ms lookup — lit by a sun and a
// moon, with the sun's and the moon's discs and a table of stars over it.
//
// **Why these lookup tables.** Bruneton and Neyret's precomputation (2008) is exact to its tables
// but four-dimensional for single scattering and three passes deep for the orders after it, and has
// to be redone whenever the air changes; Hillaire's is four small tables of which only two depend
// on the air (the transmittance, 256 x 64, and the multiple-scattering transfer, 32 x 32, built
// once a scene), and two on the frame (the sky as seen from the eye, 192 x 108, and the air between
// the eye and the ground, 64 x 32 x 32), which cost tens of microseconds and so follow a sun and a
// moon that move every frame. The price is that multiple scattering is isotropic past the second
// order, which Hillaire measured at under a percent of luminance for Earth's air and which puts the
// sky within the reference's noise at the scale this renderer compares at. It is also what every
// engine the owner could hold this against ships.
//
// **Every table is a buffer read through an address, not an image.** A shader that evaluates the
// sky
// (`sky.slang`) then needs no binding at all — the resolve, the reference path tracer and the
// tables' own passes include it as they include `brdf.slang` — and its bilinear reads are
// arithmetic the CPU mirror (`domain/gfx/tests/sky_reference.h`) repeats in double precision, so a
// test holds the GPU to it at a stated tolerance rather than to a filter unit's eight-bit weights.
//
// **Units.** Lengths are kilometres inside the atmosphere and metres in the world: a world point is
// `(x, y, z) / 1000` above a planet whose surface is the world's `y = 0`, and the world is flat
// (its up is +y everywhere), which over the kilometres a scene spans tilts nothing by more than a
// few hundredths of a degree. Radiance and illuminance are **sun units**: the sun's illuminance
// outside the atmosphere at one astronomical unit is 1, which is `k_sky_sun_lux` lux; the moon's is
// about 1.6e-6 of it at full, a star of magnitude 0 about 2e-11. A float holds all of it. What a
// display shows is chosen by the exposure (`SkyParams::exposure`, `SkyFrame::exposure`).

#include <core/base/types.h>
#include <core/math/math.h>

#include <cmath>

namespace engine::gfx {

// ---- the tables --------------------------------------------------------------------------------

// The transmittance to the top of the atmosphere, by the ray's start radius and the cosine of its
// zenith angle, in Bruneton's parameterization (the distance to the top, spread between its least
// and its most at a radius), which puts texels where the transmittance changes fastest: at the
// horizon.
inline constexpr u32 k_sky_transmittance_width = 256;
inline constexpr u32 k_sky_transmittance_height = 64;
// Hillaire's Psi_ms: the light every order of scattering past the first brings to a point, per unit
// of the light's illuminance and of the point's scattering coefficient, by the sun's zenith cosine
// and the point's height.
inline constexpr u32 k_sky_multiscatter_size = 32;
// The sky from the eye: azimuth (world, from +x towards +z) across, the zenith angle down with rows
// crowded towards the horizon (Hillaire's quadratic), which is where the sky changes fastest.
inline constexpr u32 k_sky_view_width = 192;
inline constexpr u32 k_sky_view_height = 108;
// The air between the eye and a surface: the sky-view table's two angles with a third axis, the
// distance, in `k_sky_aerial_depth` slices a quadratic apart out to `k_sky_aerial_distance_km`.
// **In directions rather than in the frustum**, where Hillaire has it, because the frame has up to
// eight views (a surround's three, a Panini's oversampled source) and one table of directions
// serves every one of them, and because it is then the sky-view table's own parameterization with
// depth, which is what makes the air at the far end of a ray the sky beyond it.
inline constexpr u32 k_sky_aerial_width = 64;
inline constexpr u32 k_sky_aerial_height = 32;
inline constexpr u32 k_sky_aerial_depth = 32;
inline constexpr f32 k_sky_aerial_distance_km = 32.0f;
// The sky's ambient term: nine spherical-harmonic coefficients, one workgroup, `k_sky_sh_rows` rows
// of zenith angle by twice as many of azimuth over the upper hemisphere.
inline constexpr u32 k_sky_sh_rows = 32;

// The lowest a camera stands above the planet in the tables: a ray from an eye at 1.7 m and one
// from 10 m see the same sky, and a float radius of 6,360 km cannot tell the two apart anyway (its
// step is half a metre). Hillaire's `PLANET_RADIUS_OFFSET`.
inline constexpr f32 k_sky_min_altitude_km = 0.01f;

// Lux in one sun unit: the sun's illuminance outside the atmosphere at one astronomical unit (the
// solar illuminance constant, about 128 klux). What the exposure rule's lux are counted in.
inline constexpr f32 k_sky_sun_lux = 128000.0f;

// The views one frame's sky is seen through (a surround's three, and room): the same bound as the
// renderer's `k_max_views`, which asserts it.
inline constexpr u32 k_sky_max_views = 8;

// The star table's lookup: a cube over the celestial sphere, `k_sky_star_face_cells` cells a side
// of each face, each cell listing the stars whose drawn spot can reach it.
inline constexpr u32 k_sky_star_face_cells = 32;
inline constexpr u32 k_sky_star_cells = 6 * k_sky_star_face_cells * k_sky_star_face_cells;
// A star's spot is a Gaussian a pixel wide, clamped to this many radians (0.1 degrees), which is
// how far from a cell a star is listed in it: three of the widest spot's sigma.
inline constexpr f32 k_sky_star_max_sigma = 1.745329e-3f;
inline constexpr f32 k_sky_star_reach = 3.0f * k_sky_star_max_sigma;

// SkyParams::flags.
inline constexpr u32 k_sky_moon = 1u;      // the moon lights the sky and is drawn
inline constexpr u32 k_sky_stars = 2u;     // the star table is drawn
inline constexpr u32 k_sky_sun_disc = 4u;  // the sun's disc is drawn

// SkyParams::exposure.x.
inline constexpr f32 k_sky_exposure_auto = 0.0f;
inline constexpr f32 k_sky_exposure_fixed = 1.0f;

// **The display's shoulder** (`sky_tone` in sky.slang; renderer.md, "Exposure"). The stand-in's
// display transform is the 1/2.2 power and a clip at white, which never mattered under a sun of
// intensity 1; under a physical sky the sun's disc is ten thousand times the sky and the aureole
// round it and a dusk's horizon several times the ground the exposure is metered from, and a clip
// turned each into a flat white blob that took a third of the picture. So an exposed channel at or
// below `k_sky_shoulder` is shown exactly as before and one above it rolls off towards white,
// `s + (1 - s)(1 - exp(-(x - s) / (1 - s)))`, which meets the line with the same slope. Per
// channel, as film does: a bright orange sky desaturates towards yellow and white as it brightens.
// Only a scene with a sky has it; `renderer.sky.shoulder` of 1 is the clip.
inline constexpr f32 k_sky_shoulder = 0.6f;

// One view the frame's sky is seen through: what turns a pixel into the direction it looks along.
// Mirrors SkyView in sky.slang. 80 bytes.
struct SkyView {
  // Clip space to the world direction of the eye's ray: the inverse of the projection times the
  // view's rotation, with no translation (view_ray.h says why not the inverse view-projection).
  Mat4 clip_to_ray;
  // x: radians one pixel spans at the view's centre (a star's spot is that wide); yzw unused.
  Vec4 pixel{};
};
static_assert(sizeof(SkyView) == 80);

// The frame's sky, written by the CPU once a frame and read by the tables' passes, the resolve
// (`ResolveParams::sky_params`) and the reference path tracer (`PathTraceParams::sky_params`).
// Mirrors SkyParams in sky.slang. 1008 bytes.
struct SkyParams {
  // ---- the air: kilometres and inverse kilometres ----
  Vec4 rayleigh{};        // rgb scattering at the ground, w scale height
  Vec4 mie_scattering{};  // rgb scattering at the ground, w scale height
  Vec4 mie_extinction{};  // rgb extinction at the ground, w the phase function's asymmetry g
  Vec4 ozone{};           // rgb absorption at the layer's peak, w the peak's height
  Vec4 radii{};  // x the planet's radius, y the atmosphere's top, z the ozone layer's half width
  // rgb: the planet's ground as the air sees it — the region hundreds of kilometres round, which
  // the multiple scattering and the sky below the horizon take (the scene's `Sky.ground_albedo`).
  Vec4 ground_albedo{};
  // rgb: the ground round the point, the ambient term's lower half (the scene's own ground albedo,
  // a terrain's sand): not the air's, since a bright dune is not the region.
  Vec4 local_albedo{};
  Vec4 night{};  // rgb: the night sky's own glow (airglow, zodiacal light), sun units / sr
  // ---- the lights ----
  Vec4 sun{};               // xyz towards the sun, w its angular radius, radians
  Vec4 sun_illuminance{};   // rgb outside the atmosphere, sun units
  Vec4 moon{};              // xyz towards the moon, w its angular radius
  Vec4 moon_illuminance{};  // rgb outside the atmosphere (lights the sky and the ground), w its
                            // albedo
  // ---- the eye and the heavens ----
  // xyz zero, the eye being the frame's origin (ADR-0053); w its altitude in the tables, km. The
  // shaders read only w:
  // the sky is a function of the eye's height, and no direction is made from where it stands
  // (view_ray.h).
  Vec4 camera{};
  // The world directions of the celestial frame's axes (x towards the vernal equinox, z the north
  // celestial pole): a star's world direction is `x * d.x + y * d.y + z * d.z`.
  Vec4 celestial_x{};
  Vec4 celestial_y{};
  Vec4 celestial_z{};
  // x k_sky_exposure_auto or _fixed, y the EV100 (fixed) or the stops added to the rule's (auto),
  // z the knee below which the rule stops compensating fully, lux, w the slope below it.
  Vec4 exposure{};
  // x the stars in the table, y their brightness scale (1), zw unused.
  Vec4 stars{};
  // ---- the tables and the frame's sums (float4 arrays through addresses) ----
  u64 transmittance = 0;  // float4[256 * 64]: rgb transmittance
  u64 multiscatter = 0;   // float4[32 * 32]: rgb Psi_ms
  u64 sky_view = 0;       // float4[192 * 108]: rgb radiance
  u64 aerial = 0;         // float4[64 * 32 * 32]: rgb in-scattered radiance, a mean transmittance
  u64 frame = 0;       // SkyFrame*: written by `sky_frame_main`, read by the resolve and the tracer
  u64 star_list = 0;   // float4[2 * count]: celestial direction (xyz), then rgb irradiance
  u64 star_cells = 0;  // uint[k_sky_star_cells + 1]: each cell's first index into star_index
  u64 star_index = 0;  // uint[]: the stars each cell lists
  u32 flags = 0;       // k_sky_moon | k_sky_stars | k_sky_sun_disc
  u32 view_count = 0;
  // Where the display's shoulder starts (`sky_tone`): exposed values below it are shown as they
  // are, above it they roll off towards white instead of clipping. 1 is no shoulder: a clip.
  f32 shoulder = k_sky_shoulder;
  u32 pad = 0;
  SkyView views[k_sky_max_views];
};
static_assert(sizeof(SkyParams) == 1008);
static_assert(sizeof(SkyParams) % 16 == 0, "the block is read as float4 rows on the GPU");

// What the frame's sky pass works out once for every pixel to read: the ambient term's
// coefficients, the ground's radiance, the lights at the eye and the exposure. Written on the GPU
// by `sky_frame_main` (sky_luts.slang) from the frame's tables, read by the resolve and the
// reference. Mirrors SkyFrame in sky.slang. 208 bytes.
struct SkyFrame {
  // The ambient radiance a Lambertian surface of normal n takes, `max(sum sh[i] * Y_i(n), 0)`: the
  // environment's L2 spherical harmonics (the sky above the horizon, the ground below it) already
  // convolved with the cosine lobe and divided by pi (Ramamoorthi and Hanrahan 2001), rgb.
  Vec4 sh[9];
  Vec4 ground{};  // rgb: the ground's radiance below the horizon (the ambient's lower half)
  Vec4
      sun_ground{};  // rgb: the sun's illuminance at the eye, after the air; w its elevation's sine
  Vec4 moon_ground{};  // the same for the moon
  // x what one sun unit of radiance is on the display (before the transfer curve), y the EV100 it
  // came from, z the illuminance it was metered from (lux: the sun and the moon on a level surface
  // at the eye, and pi times the sky's mean radiance), w the sky's share of that (lux).
  Vec4 exposure{};
};
static_assert(sizeof(SkyFrame) == 208);

// ---- the exposure rule (renderer.md, "Exposure") ------------------------------------------------
//
// Incident-light metering at ISO 100: the exposure value a meter with the usual calibration
// constant (C = 250) reads for an illuminance E is `EV100 = log2(E / 2.5)` (E in lux), and the
// exposure it intends renders a grey card of 18% as the display's middle grey, 0.18 linear — which
// puts a white Lambertian surface lit by E (luminance E / pi) at 1.0, where the display's shoulder
// (`k_sky_shoulder`) shows it at 0.85, whatever E is: daylight at 100 klux and moonlight at 0.2 lux
// alike. That is right for a photograph and wrong for an eye, which
// stops compensating as the light fails, so below a knee (`knee_lux`, the renderer's
// `renderer.sky.exposure_knee_lux`, 40 klux by default: a clear sun about 20 degrees up) each
// further stop of darkness is compensated by only `slope` of a stop (`renderer.sky.exposure_slope`,
// 3/4): a sunset is darker than noon, a moonlit desert reads as night, and a moonless one darker
// still. `compensation` stops are subtracted from the result (a positive one brightens). E is the
// sun's and the moon's illuminance on a level surface at the eye plus pi times the sky's *mean*
// radiance over its dome — not the sky's level illuminance, which weighs each part of the sky by
// its elevation's sine and so reads a dusk whose glow is all at the horizon as darker than it
// looks. Shared by the shader (`sky_exposure_ev100`) and the CPU (`sky_exposure_ev100` below),
// which the renderer's summary and the tests report with.
inline f64 sky_exposure_ev100(f64 lux, f64 compensation, f64 knee_lux, f64 slope) noexcept {
  const f64 e = lux > 1.0e-12 ? lux : 1.0e-12;
  const f64 knee = knee_lux > 1.0e-12 ? knee_lux : 1.0e-12;
  f64 ev = std::log2(e / 2.5);
  if (e < knee) ev = std::log2(knee / 2.5) + slope * std::log2(e / knee);
  return ev - compensation;
}
// What one sun unit of radiance is on the display, before the shoulder and the transfer curve: the
// luminance in cd/m^2 (`k_sky_sun_lux` times the radiance) over the luminance of a white Lambertian
// surface under the illuminance that EV100 meters, 2.5 2^EV100 / pi.
inline f64 sky_exposure_scale(f64 ev100) noexcept {
  return static_cast<f64>(k_sky_sun_lux) * 3.14159265358979323846 / (2.5 * std::exp2(ev100));
}

}  // namespace engine::gfx
