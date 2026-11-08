#pragma once

// **Channels of flowing water** (docs/subsystems/water.md, "Channels"; roadmap S96; the grounding,
// docs/experiments/flowing-water.md): a scene's `water_channels` (`engine.scene.WaterChannel`) as
// ribbons of water down their centrelines, with the speed of the water at every point from
// Manning's equation and the stretches that fall at 45 degrees or steeper as waterfalls.
//
// **What a vertex carries** (no new attribute: the body's GLB layout, so a glTF reader that takes
// one UV set is enough). Positions are metres from the channel's origin (`Channel::origin`, where
// its instance stands, unrotated). The normal is the ribbon's. **UV.y is the depth under the
// vertex**, as a body's is (the bed a parabola across the width, zero past it). **UV.x is the flow
// phase**: the travel time from the head of the channel, seconds, `tau = integral ds / v(s)` along
// the centreline, constant across a section — so a pattern drawn as a function of `tau - t` moves
// downstream at the local speed `v`, whatever the speed is, and the speed and direction are the
// inverse length and the direction of the phase's gradient, which the resolve recovers from a
// triangle's three corners (water.slang, `water_channel_flow`). The phase **wraps** every
// `k_channel_phase_period_s` seconds, so a half-float UV keeps 4 ms of it at the top of its range;
// at the wrap the section is written twice, once ending the strip above at `tau` and once starting
// the strip below at `tau - period`, which is a UV seam the cluster simplifier protects, and every
// pattern the resolve draws is periodic in the phase with that period.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <systems/water/water.h>

#include <cmath>
#include <schemas/scene.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::water {

// The placement generator the renderer's scene reader hands a scene's `water_channels` to.
inline constexpr std::string_view k_channel_generator = "water_channel";
// The channel mesh builder's version: part of every channel file's bytes.
inline constexpr u32 k_channel_mesh_version = 1;
// The flow phase's period, seconds (water.slang's `k_channel_period_s`): UV.x lies in
// [0, period + one strip's travel time).
inline constexpr f32 k_channel_phase_period_s = 8.0f;
// A stretch whose drop over its horizontal run is this or more (45 degrees) is a waterfall: the
// water leaves the bed and falls freely (docs/experiments/flowing-water.md, "Falls").
inline constexpr f32 k_channel_fall_gradient = 1.0f;
// How far past each edge of the width the ribbon goes, as a share of the width: the ground clips
// the surface where it rises over the bank, so the shore is where the ground says, not where the
// description's width ends.
inline constexpr f32 k_channel_edge_extend = 0.25f;
// The slowest water a channel carries, m/s: Manning's speed with a slope of 0 is still water,
// whose phase would never advance.
inline constexpr f32 k_channel_min_speed = 0.05f;
// The plunge below a fall: the jet's speed decays back to the channel's over a length that is
// this share of the fall's height, within [k_channel_plunge_min_m, k_channel_plunge_max_m] — a
// choice, stated on the grounding page.
inline constexpr f32 k_channel_plunge_share = 0.4f;
inline constexpr f32 k_channel_plunge_min_m = 1.0f;
inline constexpr f32 k_channel_plunge_max_m = 15.0f;
inline constexpr f64 k_gravity = 9.81;
// **The brink** (S98; flowing-water.md, "The lip"): a subcritical reach that ends at a fall
// accelerates into it — the depth falls from the critical depth d_c = (q^2 / g)^(1/3) some three to
// four d_c upstream to Rouse's brink depth 0.715 d_c at the lip — so the last
// k_channel_brink_reach d_c of the reach speed up to q / (0.715 d_c), quadratically, and the fall
// leaves the lip at that speed. A supercritical reach goes over as it is.
inline constexpr f64 k_channel_brink_depth_share = 0.715;
inline constexpr f64 k_channel_brink_reach = 4.0;

// **The surface's regimes** (S98; flowing-water.md, "What the surface does"): the numbers the
// resolve's channel shading (water.slang, "channels of flowing water") is drawn with, here so the
// tests hold them to their sources; the shader mirrors them by name.
//
// Brocchini and Peregrine 2001: turbulence of velocity scale q and length scale L disturbs the
// surface against gravity where q^2 > 2 g L and against surface tension where q^2 > 2 sigma / (rho
// L); the two lines cross at the capillary length sqrt(sigma / (rho g)). Below both the surface is
// smooth with deformations of height about q^2 / 2g, which the shader draws, with q the shear
// velocity u* = v n sqrt(g) / d^(1/6) (Manning's).
inline constexpr f64 k_water_surface_tension = 0.0728;  // N/m, water at 20 C
inline constexpr f64 k_water_density = 998.2;           // kg/m^3, water at 20 C
inline f64 capillary_length_m() noexcept {
  return std::sqrt(k_water_surface_tension / (k_water_density * k_gravity));
}
// The surface's deformation height, metres, for turbulence of velocity scale q (m/s).
inline f64 surface_disturbance_m(f64 q) noexcept { return q * q / (2.0 * k_gravity); }
// The shear velocity of a channel of mean speed v and depth d at Manning's n.
f64 shear_velocity(f64 speed, f64 depth_m, f64 n) noexcept;
// Self-aeration (Chanson; Cummings and Chanson's inception velocities 0.74 to 1.58 m/s): from the
// critical Froude number at a speed of a metre a second (water.slang's
// `k_channel_aeration_speed`).
inline constexpr f64 k_channel_aeration_speed = 1.0;
// Standing waves (Kennedy 1963): stationary in the channel's frame at the wavelength whose wave
// speed is the flow's, 2 pi v^2 / g, standing where the Froude number is within about [0.8, 1.6].
inline f64 standing_wavelength_m(f64 speed) noexcept {
  return 2.0 * 3.14159265358979323846 * speed * speed / k_gravity;
}
inline constexpr f64 k_channel_wave_froude_lo = 0.8;
inline constexpr f64 k_channel_wave_froude_hi = 1.6;
// The lateral profile: the speed across the width is the 1/6 power law's (Manning's own exponent,
// Chen 1991), v(a) = C v_mean (1 - (2a/w)^2)^(1/6) over the parabolic bed, C making the
// discharge the section's mean speed times its area: (4/3) / integral_-1^1 (1 - x^2)^(7/6) dx.
inline constexpr f64 k_channel_profile_power = 1.0 / 6.0;
inline constexpr f64 k_channel_profile_mean = 1.045718;
// The speed at the share r = 1 - (2a/w)^2 of the centreline's depth, as a share of the mean.
inline f64 lateral_speed_share(f64 r) noexcept {
  return r > 0.0 ? k_channel_profile_mean * std::pow(r, k_channel_profile_power) : 0.0;
}
struct Channel {
  std::string name;
  // Where the instance stands: the centre of the points' bounding box in x and z, and the first
  // point's surface height (ADR-0053: the mesh is metres from here).
  WorldPos origin{};
  // The points less the origin, metres.
  Vector<Vec3> points;
  Vector<f32> width_m;
  Vector<f32> depth_m;
  Vector<f32> slope;
  f32 manning_n = 0.04f;
  f32 shore_m = 2.0f;
  u64 seed = 1;
  // The channel's last point is a fall's foot that lands in a water body (the renderer's reader
  // says so, through the generator's "pooled" list): the body draws the plunge, and the ribbon
  // builds no plunge stretch over the pool (with_plunge_stretch).
  bool pooled = false;
};

// The description, checked: at least two points, the arrays the points' length, every width and
// depth positive and finite, no slope negative where the line descends, Manning's n positive.
// False with a sentence naming the channel.
bool read_channel(const scene::WaterChannel& block, Channel& out, std::string* error);

// The cube root of `a` (0 for a <= 0) from + - * / alone, the same bits on every toolchain.
f64 cube_root(f64 a) noexcept;
// Manning's mean speed, m/s, for a wide channel (the hydraulic radius about the depth):
// (1/n) d^(2/3) S^(1/2), never below `k_channel_min_speed`.
f32 manning_speed(f32 n, f32 depth_m, f32 slope) noexcept;
// The Froude number v / sqrt(g d): above 1 the flow is supercritical — rapids, standing waves.
f32 froude(f32 speed, f32 depth_m) noexcept;
// Whether the stretch from point `i` to point `i + 1` is a waterfall.
bool stretch_falls(const Channel& channel, u32 i) noexcept;
// **The plunge stretch** (S98): a channel whose last stretch falls and that is not `pooled` ends at
// the fall's foot with nothing below it for the plunge's white water; this returns it with one more
// point, the plunge's length (`k_channel_plunge_share` of the fall's height, within its bounds)
// on along the heading at the foot's height, the last width and depth, slope 0 — so the jet's
// speed decays over it as below a fall that continues. Any other channel comes back as it is.
// `build_channel_mesh` and `build_channel_depth_map` both take the channel through it.
Channel with_plunge_stretch(const Channel& channel);

// **The ribbon** (water.md, "Channels"): sections every `spacing` metres along each stretch, each
// `columns` quads across the width and `k_channel_edge_extend` of it past either edge, the surface
// at the points' heights interpolated along the line. `section_*` are per section (a wrapped
// section once, as its first copy): the station along the line (metres of the 3-D line from the
// head), the water's speed there and its travel time from the head (unwrapped).
struct ChannelMesh {
  Mesh mesh;  // `edge` the vertices a section, `rings` the sections written (wraps twice)
  u32 columns = 0;
  Vector<f32> section_s;
  Vector<f32> section_speed;
  Vector<f64> section_tau;
  Vector<u8> section_falls;  // 1: the strip below this section is a fall
};
f32 channel_spacing(const Channel& channel) noexcept;
void build_channel_mesh(const Channel& channel, ChannelMesh& out);

// The depth map (water.md, "The depth map"), over the ribbon's bounding square at a quarter of its
// spacing, from the description alone: each texel the deepest of the stretches' parabolic beds
// there. The ground is not sampled — the bed is the description's (W86 writes it from the ground's
// drainage), and where the ground stands above the surface the depth test hides the water.
void build_channel_depth_map(const Channel& channel, const ChannelMesh& mesh, DepthMap& out);

// The generator's parameters as the reader hands them: `{"channels": [<WaterChannel>...]}`. Its
// `expand` makes one mesh and one placement per channel, in order.
JsonValue channel_generator_params(std::span<const scene::WaterChannel> channels);

}  // namespace engine::water
