#pragma once

// The lights a frame is drawn with (docs/subsystems/renderer.md). One sun, one sky, and the two
// point lights that orbit the scene, in one function because **two integrators have to agree
// about them or a comparison measures the disagreement**: `SceneRenderer::record_frame` fills a
// `gfx::ResolveParams` from this and `ReferenceRenderer` fills a `gfx::PathTraceParams` from the
// same call, so the reference path tracer and the real-time resolve light the same scene with the
// same numbers at the same frame index (docs/plan/04-renderer.md §4.8).
//
// These are the renderer's own stand-in lights until a scene carries authored ones: the sun stands
// where `RenderSettings` or the `renderer.sun.*` tunables put it — or, when its day runs, moves
// from there along its daily arc (`sun_on_arc`, below) — the sky is a constant, and the two point
// lights — one warm, one cool — stand still, or with `orbit_lights` orbit out of phase as the frame
// index advances so the BSDF's specular response sweeps across a surface. **Nothing moves with the
// frame by default** (since 2026-09-27): the orbit was frame-driven, and a time-lapse's sand, lit
// by a warm light a scene's radius away that went round it every eight seconds, read as a sun
// racing across the sky (renderer.md, "The dunes in time-lapse"). The day is the caller's clock,
// not the frame's: a frame is told how far into the day it is (`FrameDesc::sun_time_s`), and 0 is
// the sun it always had. Reach and intensity scale with the scene's radius, intensity with its
// square because the falloff is inverse square, so a 2 cm mesh and a 20 m heightfield look alike.
// The ground they light is the scene's own, `SceneData::ground_albedo`: a terrain's sand, or a
// neutral grey for a scene without one.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

#include <optional>

namespace engine::renderer {

struct SceneData;
struct RenderSettings;

// The sun every frame had before it could be moved, normalize(0.4, 0.8, 0.45): 48.37 degrees from
// +x towards +z, 53.03 above the horizon. The tunables' defaults are these, and a sun at them is
// this vector to the bit, so a picture that did not ask for a sun is the picture it always was.
inline constexpr f64 k_default_sun_azimuth_deg = 48.3664606634298;
inline constexpr f64 k_default_sun_elevation_deg = 53.034893494453584;

// The direction towards the sun, azimuth in the ground plane from +x towards +z and elevation above
// it, degrees; exactly the old fixed vector at the defaults.
Vec3 sun_direction(f64 azimuth_deg, f64 elevation_deg) noexcept;

// **The sun's day** (renderer.md, "The sun's day"; engine-view's `--sun-rate` and `[` / `]`). The
// sun turns once a game day round the celestial pole, which stands `tilt_deg` above the northern
// horizon — north is -z, the way the camera paths and `--start` call "looking north", so east is
// +x and south +z — and it goes round on **the circle through its start** (`--sun`, or the
// `renderer.sun.*` tunables): whatever the start is, it is on the day's arc, which is what lets a
// day that has not moved be exactly the sun a frame had before there was a day. The circle's
// distance from the pole is the sun's declination, so the season is whatever the start implies:
// the default start (south-east, 53° up) at the default tilt is a mid-morning in April
// (declination 9.75°), whose sun culminates due south at 59.75° and sets 12.8° north of west after
// 13.1 hours above the horizon (sun_arc_tests.cpp prints them).
//
// The tilt is the latitude: 0 is the equator, where the day's circle stands upright and the sun of
// an equinox crosses the zenith; 90 a pole, where the sun circles at one elevation all day. It is
// `renderer.sun.arc_tilt_deg`, 40 by default, a mid-latitude — about where the Taklamakan and the
// Gobi are, and ten degrees north of the Sahara's middle.
inline constexpr f64 k_default_sun_arc_tilt_deg = 40.0;
inline constexpr f64 k_sun_day_s = 86'400.0;  // one turn of the arc, game seconds
struct SunArc {
  f64 azimuth_deg = k_default_sun_azimuth_deg;  // where the sun stands at the start of its day
  f64 elevation_deg = k_default_sun_elevation_deg;
  f64 tilt_deg = k_default_sun_arc_tilt_deg;  // the pole's elevation over the northern horizon
};
// A request's arc: the start from `RenderSettings::sun_*`, or the `renderer.sun.azimuth_deg` /
// `elevation_deg` tunables' when it names none, and the tilt from `renderer.sun.arc_tilt_deg`.
SunArc sun_arc(const RenderSettings& settings);
// The direction towards the sun `time_s` game seconds into its day: the start turned about the
// pole by a turn a day, westward — a morning sun climbs towards the south, an evening one sinks
// towards the west and below the horizon. **At `time_s == 0` it is `sun_direction(azimuth,
// elevation)` to the bit**, so a day with a rate of zero draws what every frame drew before the
// day existed; elsewhere it is evaluated in f64 and is continuous in the time.
Vec3 sun_on_arc(const SunArc& arc, f64 time_s) noexcept;
// How strongly a sun in `direction` shines: 1 at and above the horizon, fading to nothing
// `k_sun_twilight_deg` below it (a smoothstep in the sine of the elevation, which is the
// direction's y). The sun's colour is left alone, and so are the sky and the point lights: there is
// no sunset red and no night sky (renderer.md, "The sun's day", says why).
inline constexpr f64 k_sun_twilight_deg = 6.0;
f32 sun_intensity(const Vec3& direction) noexcept;
// `renderer.sun.rate`: game seconds per real second the sun's day runs at, 0 (a still sun) by
// default. The host's to read and to integrate — engine-view's `--sun-rate` and its `[` / `]` —
// because the renderer is handed how far into the day a frame is, never how fast the day runs.
f64 sun_rate_tunable();

// What `frame_lighting` lights with: the point lights or not, orbiting or not, and the sun and how
// strongly it shines (`FrameLighting::sun.w`).
struct LightingOptions {
  bool lights = true;
  bool orbit = false;
  Vec3 sun = sun_direction(k_default_sun_azimuth_deg, k_default_sun_elevation_deg);
  f32 sun_intensity = 1.0f;
};
// A request's, `sun_time_s` game seconds into the sun's day (`sun_arc`, `sun_on_arc`): at 0 its sun
// is the start and shines at 1 wherever the start is, which is what every frame had before there
// was a day; later the sun is on the arc and shines by `sun_intensity`. (A start below the horizon
// is therefore lit at 1 until the day moves it and by the rule after, which is a step once, at the
// first frame of the day; a start at or above the horizon has none.)
LightingOptions lighting_options(const RenderSettings& settings, f64 sun_time_s = 0.0);

// The two point lights beside the sun.
inline constexpr u32 k_frame_lights = 2;

// The sky, in one place, because **three** things have to be the same number. It is the resolve
// pass's clear value and `gfx::ResolveParams::sky`, which `sky_is_clear` ties together — the
// shader discards an empty pixel instead of writing a colour the clear already put there, so two
// spellings would make an uncovered pixel take whichever the clear said, silently. And it is the
// reference path tracer's background, quantized on the CPU from this constant: a third spelling
// would put a one-byte difference on every empty pixel of every comparison, which is a mistake
// already made once here and measured (docs/subsystems/renderer.md, "Reference renderer").
inline constexpr Vec4 k_sky{0.55f, 0.70f, 0.90f, 1.0f};

// **How far a ray leaves a surface** (`FrameLighting::shadow_bias` and `shadow_bias_steps`;
// `ray_offset` in material.slang; docs/subsystems/renderer.md, "Shadows"), in two parts because
// there are two gaps between the surface a pixel is rebuilt on and the one a ray is traced against:
//
// - **One step of the surface's own 16-bit grid**, through its instance's largest scale. Rounding
//   to the grid moves a vertex at most half a step on each axis, so √3/2 of a step along any
//   normal; one step clears it. It is the receiver's own, so the 5 km terrain gets 7.8 cm and a
//   4 m wall standing on it 0.06 mm.
// - **2^-18 of the scene's reach** (the distance from the origin to the far side of its bounds,
//   which no coordinate exceeds), for the float arithmetic the grid says nothing about: a world
//   position is a matrix product of the grid's floats and the ray is transformed back into the
//   instance's space. 2^-18 is 32 to 64 ulps of the largest coordinate; 1.8 cm on the ashlar ruins'
//   5 km scene, 4 µm on a Khronos sample a metre across.
//
// Measured on the ruins at frame 900 of the owner's 15:26 recording (renderer.md): the terrain
// with no grid term is acne over 2.3 million pixels, the kit with no float term speckles, and both
// together leave the rays and the cascaded maps disagreeing on 6,445 of 8.3 million pixels, every
// one at an edge or in a dent the maps' texels cannot hold.
inline constexpr f32 k_shadow_bias_steps = 1.0f;
inline constexpr f32 k_shadow_bias_relative = 1.0f / 262144.0f;

struct FrameLighting {
  Vec4 sky{};  // rgb: the background an uncovered pixel shows, and the hemisphere's upper half
  Vec4 sun{};  // xyz normalized direction towards the light, w intensity
  // rgb: the ground's albedo, `SceneData::ground_albedo` — the hemisphere's lower half is this
  // ground lit by `sun` and `sky` (gfx::ResolveParams::ground, PathTraceParams::ground). w unused.
  Vec4 ground{};
  gfx::ResolveLight lights[k_frame_lights];
  u32 light_count = 0;
  // How far a ray leaves a surface along its geometric normal: `shadow_bias` world units plus
  // `shadow_bias_steps` steps of **the surface's own** 16-bit position grid (`ray_offset` in
  // material.slang; gfx::ResolveParams and gfx::PathTraceParams carry both). The grid term is the
  // gap between the surface a pixel or a hit is rebuilt on and the float one the acceleration
  // structures hold; the world term is float error, sized by the scene's reach. Until 2026-09-26
  // it was one number, a thousandth of the scene's radius, which is 4.1 m over the ashlar ruins'
  // 5 km terrain: every ray from the sand started above every wall (renderer.md, "Shadows").
  f32 shadow_bias = 0.0f;
  f32 shadow_bias_steps = 0.0f;
};

// With `orbit`, `frame_index` drives the point lights' orbit, exactly as it drives the deformation
// phase, so frame N of a capture and frame N of a reference render are lit identically; without it
// they stand where frame 0 puts them. `lights` false leaves the two point lights out and the count
// at zero, which is what `--no-lights` asks for. The sun is `options.sun` shining at
// `options.sun_intensity`.
void frame_lighting(const SceneData& scene, u64 frame_index, const LightingOptions& options,
                    FrameLighting& out);

// **With a sky** (sky.h; renderer.md, "The sky"): the sky's lights in place of the stand-in's. The
// sun is the provider's at the frame's time and shines at 1 — the resolve and the reference take
// its illuminance and colour from the sky through the air, and a sun under the planet's horizon is
// dark because the air says so, not by a fade. The moon, when the scene's is on, is **a directional
// light of the array** (`gfx::k_light_directional`), its illuminance outside the air, which the
// resolve also dims by the air along it and shadows like the sun: a ray in `--shadows rt`, and the
// cascaded maps when it is the key light of a night (`FrameSky::moon_key`). The stand-in's two
// point lights are left out, whatever `lights` says: they stand for a scene's authored lights, of
// which a sky's scene has none, and a light four radii wide at the brightness of the stand-in sun
// would be noon at midnight. The ground and the rays' offsets are the stand-in's.
struct FrameSky;
void frame_lighting(const SceneData& scene, const FrameSky& sky, FrameLighting& out);

}  // namespace engine::renderer
