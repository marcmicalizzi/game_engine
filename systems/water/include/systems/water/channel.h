#pragma once

// **Channels of flowing water** (docs/subsystems/water.md, "Channels"; roadmap S96, S106; the
// grounding, docs/experiments/flowing-water.md and docs/experiments/stream-profile.md): a scene's
// `water_channels` (`engine.scene.WaterChannel`, version 6: a **solved** description, W120) drawn
// as the surface the solve found. At every station the description says the level, the wet reach
// either side of the line, the section's mean speed and its Froude number; this file makes the
// sheet of water that lies between the wet edges at that level, its depth map from the ground
// under it, and the numbers the resolve shades it by. Nothing here derives a level, a width or a
// speed: a description that is not solved is refused (`read_channel`).
//
// **The sheet** (`build_channel_mesh`). Cells of a square lattice, world-aligned, clipped by
// marching squares to the region `inside > 0` — within the wet reach plus a margin under the bank
// of the nearest place on the line, between the ends of a run of drawn stretches — every vertex at
// the level of its own nearest place on the line, so the sheet is level across the line and the
// line's straight interpolation along it. One lattice, so **no two triangles overlap in plan**
// however the line bends (rows across the line fold where a bend is tighter than the half width).
// **The visible edge is where the ground meets the level**: the sheet runs `k_channel_under_bank_m`
// past the wet edge, under ground that by the wet edge's definition stands above the level there,
// and the depth test hides it.
//
// **What a vertex carries** (no new attribute: the body's GLB layout, so a glTF reader that takes
// one UV set is enough). Positions are metres from the channel's origin (`Channel::origin`, where
// its instance stands, unrotated). **UV.x is the flow phase**: the travel time from the head of
// the channel at the section's mean speed, seconds, constant across a section — so a pattern drawn
// as a function of `tau - t` moves downstream at the local speed, and the speed and direction are
// the inverse length and the direction of the phase's gradient, which the resolve recovers from a
// triangle's three corners (water.slang, `water_channel_flow`). The phase **wraps** every
// `k_channel_phase_period_s` seconds: a triangle takes its corners' phases less the whole periods
// under its least, so a vertex on a wrap is written once for each side, a UV seam the cluster
// simplifier protects. **UV.y is the section's hydraulic depth** `A / T`, metres, which with the
// speed is the solved Froude number (`V / sqrt(g A / T)`); the depth under a pixel is the depth
// map's. **On a fall UV.y is negative**: less the nappe's thickness, which is how the resolve
// knows a fall's triangle (any corner negative) whatever its slope.

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
// The channel mesh builder's version: part of every channel file's bytes. 2: the sheet from the
// solved description (S106).
inline constexpr u32 k_channel_mesh_version = 2;
// The flow phase's period, seconds (water.slang's `k_channel_period_s`): UV.x lies in
// [0, period + one triangle's travel time).
inline constexpr f32 k_channel_phase_period_s = 8.0f;
inline constexpr f64 k_gravity = 9.81;

// ---- what the draw bounds (water.md, "The surface from the solve") ------------------------------
//
// **Under the bank**: how far past the wet edge the sheet runs, metres. The description holds the
// wet reach between two stations to 5 cm of the ground's (W120's pin) and a lattice of the ground
// is at most half a metre, so half a metre is under the bank wherever the bank rises, and short
// enough that a hollow beside the channel is rarely inside it. At a station that stands over its
// bank the margin is 0: there the wet reach ends at the bank's crest, and past it the ground falls
// away under the level.
inline constexpr f32 k_channel_under_bank_m = 0.5f;
// The sheet's lattice, metres: a quarter of the narrowest sheet (wet reach and margins), within
// these, grown by a quarter until the mesh keeps under `k_max_vertices`.
inline constexpr f32 k_channel_lattice_min_m = 0.25f;
inline constexpr f32 k_channel_lattice_max_m = 1.0f;
// The depth map's texel, metres: the finest lattice the engine's grounds are cooked at (a quarter
// of a metre), on the world's multiples of it, so over such a ground a texel is a lattice point
// and the map is the ground's own bilinear surface. Coarser where the channel's bounding square
// would pass `k_depth_map_max` texels. (A number taken from the grounds this engine cooks, not
// from the ground at hand, which `scene_gen::Ground` does not let a reader ask: water.md says so.)
inline constexpr f32 k_channel_map_texel_m = 0.25f;
// The speed the phase is written with is the section's mean, within these, m/s: still water's
// phase would never advance, and 20 m/s is a free fall of 20 m.
inline constexpr f32 k_channel_min_speed = 0.05f;
inline constexpr f32 k_channel_max_speed = 20.0f;
// **The Froude number the shader reads is at most this.** W120's thin films reach Froude numbers
// past 100; every term of the shading that reads it is saturated by 2 (the foam is whole, the
// standing waves' band ended at 1.6, the roughness's rise is whole), so a bound of twice that
// changes no pixel's regime and keeps the hydraulic depth written, V^2 / (g Fr^2), no smaller than
// V^2 / (16 g).
inline constexpr f32 k_channel_froude_max = 4.0f;
// The hydraulic depth and the nappe's thickness are never written under a millimetre (a half
// float's step at a metre is half a millimetre).
inline constexpr f32 k_channel_depth_min_m = 0.001f;

// ---- the surface's regimes (S98; flowing-water.md, "What the surface does") ---------------------
//
// The numbers the resolve's channel shading (water.slang, "channels of flowing water") is drawn
// with, here so the tests hold them to their sources; the shader mirrors them by name.
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
// **From the section's mean to the surface** (S106). Down a vertical the speed follows the 1/6
// power law (Manning's own exponent; Chen 1991), u(z) = u_s (z / d)^(1/6), whose mean over the
// depth is 6/7 of the surface's: the surface runs at 7/6 of the vertical's mean (0.857 the other
// way: the 0.85 to 0.86 a surface float's speed is multiplied by to gauge a stream, Rantz 1982).
inline constexpr f64 k_channel_surface_factor = 7.0 / 6.0;
// **Across the section** each vertical carries Manning's flow for its own depth at the section's
// one friction slope, u(d) ~ d^(2/3) (the divided-channel rule), so against the section's mean
// u(d) / V = kappa (d / D)^(2/3) with D the hydraulic depth and kappa = D^(5/3) / mean(d^(5/3)):
// 1 for a rectangle, 0.895 for a parabola, 0.84 for a triangle. The shader has D and d and no
// integral, so it takes kappa as 0.9 (a parabola's to 1 %, 10 % under a rectangle's, 7 % over a
// triangle's) and never more than `k_channel_profile_max` of the mean.
inline constexpr f64 k_channel_conveyance = 0.9;
inline constexpr f64 k_channel_profile_max = 1.5;
// The vertical's mean speed over a depth `d`, as a share of the section's mean, in a section of
// hydraulic depth `D`.
f64 lateral_speed_share(f64 d, f64 hydraulic_depth) noexcept;
// **A fall's sheet is glassy until it breaks up** (S106). A nappe of unit discharge q (m^2/s)
// falls as a coherent sheet for Horeni's (1956) break-up length 5.89 q^0.319 metres (as Ervine,
// Falvey and Withers 1997 give it) and is white water past it: a 25 cm step is glass at any
// discharge a stream carries (a litre a second a metre breaks up after 0.65 m), a 2 m fall is
// white at its foot under 34 litres a second a metre and whitening there under 170, and the
// fixture's 0.58 m^2/s holds its sheet for 4.95 m. The shader fades the white in over
// [k_channel_breakup_from, 1] of the length, from the height the sheet has fallen.
f64 nappe_breakup_m(f64 unit_discharge) noexcept;
inline constexpr f64 k_channel_breakup_from = 0.6;

// One channel as this capability draws it: the solved description about its origin, bounded.
struct Channel {
  std::string name;
  // Where the instance stands: the centre of the points' bounding box in x and z taken down to a
  // whole metre (so the sheet's lattice and the depth map's texels are the world's multiples of
  // their spacing), and the first point's level (ADR-0053: the mesh is metres from here).
  WorldPos origin{};
  // The stations less the origin, metres: y the solved level.
  Vector<Vec3> points;
  Vector<f32> depth_m;        // the solved greatest depth
  Vector<f32> speed_mps;      // the section's mean, within [k_channel_min_speed, k_channel_max_speed]
  Vector<f32> froude;         // within [0, k_channel_froude_max]
  Vector<f32> discharge_m3s;
  Vector<f32> wet_left_m;     // left is the side of (-dz, dx) for a line running (dx, dz)
  Vector<f32> wet_right_m;
  Vector<u16> flags;          // scene_gen/water_channel.h
  f32 manning_n = 0.04f;
  f32 shore_m = 2.0f;
  u64 seed = 1;
};

// The description, checked. **Solved or refused** (`scene_gen::water_channel_solved`: a block
// cooked before W120, or one that names a discharge and no solve, is refused by name), and then
// this capability's own: Manning's n positive, the optics in a body's ranges, no two neighbouring
// stations at one place in plan (a station is a place along the line; a fall's foot stands
// downstream of its lip). False with a sentence naming the channel.
bool read_channel(const scene::WaterChannel& block, Channel& out, std::string* error);

// The cube root of `a` (0 for a <= 0) from + - * / alone, the same bits on every toolchain.
f64 cube_root(f64 a) noexcept;
// Manning's mean speed, m/s, for a wide channel (the hydraulic radius about the depth):
// (1/n) d^(2/3) S^(1/2). Not read by the draw (the description's speed is the solve's); what a
// hand-written uniform reach's speed is, and what the tests write one with.
f32 manning_speed(f32 n, f32 depth_m, f32 slope) noexcept;
// The Froude number v / sqrt(g d).
f32 froude(f32 speed, f32 depth_m) noexcept;
// The hydraulic depth a station's speed and Froude number say, V^2 / (g Fr^2), metres: the bounded
// speed and Froude number's, never under `k_channel_depth_min_m`; where the Froude number is 0
// (still water) the station's greatest depth.
f32 station_hydraulic_depth(const Channel& channel, u32 i) noexcept;

// What the stretch from station `i` to station `i + 1` is.
bool stretch_falls(const Channel& channel, u32 i) noexcept;  // a lip and its foot
bool stretch_jumps(const Channel& channel, u32 i) noexcept;  // a jump's head and its foot
// **Drawn unless both its stations stand over a bank** (W120's films along lines that climb out of
// a hollow: drawing them is water going uphill).
bool stretch_drawn(const Channel& channel, u32 i) noexcept;

// **The surface at a place** (the one evaluation the mesh, the depth map and `channel_sample` all
// make): the nearest place on the line to (x, z), metres from the channel's origin — the lowest
// stretch on a tie — and the solve there.
struct ChannelAt {
  u32 stretch = 0;
  f32 t = 0.0f;           // along the stretch, [0, 1]
  f32 distance_m = 0.0f;  // from the line, in plan
  bool left = false;
  // > 0 inside the sheet: the least of how far inside the wet reach and its margin the place is and
  // how far along its run of drawn stretches from either end; < 0 outside, or on an undrawn one.
  f32 inside = -1.0f;
  f32 wet_inside = -1.0f;    // the same without the margin: > 0 within the wet reach itself
  f32 level = 0.0f;          // metres over the origin's y; on a fall the nappe's parabola
  f32 slope = 0.0f;          // d level / d metre along the heading
  Vec2 heading{1.0f, 0.0f};  // the stretch's direction in plan, unit, downstream
  f32 speed = 0.0f;          // the section's mean, m/s; on a fall the nappe's own
  f64 tau = 0.0;             // the travel time from the head, seconds, unwrapped
  // The section's hydraulic depth, metres; on a fall less the nappe's thickness (negative).
  f32 hydraulic = 0.0f;
  f32 depth_m = 0.0f;        // the solved greatest depth at the place along the line
  bool fall = false;
};
// The line's per-stretch constants, computed once: where each station is along the line in plan
// and in travel time, each stretch's heading, and the runs of drawn stretches.
struct ChannelLine {
  Vector<f32> station;  // one a station, metres along the line in plan
  Vector<f64> tau;      // one a station, seconds from the head
  Vector<Vec2> heading; // one a stretch
  Vector<f32> length;   // one a stretch, in plan
  Vector<f32> run_from; // one a stretch: the station its run of drawn stretches starts at
  Vector<f32> run_to;   // ... and ends at (both 0 for a stretch that is not drawn)
  Vector<f32> hydraulic;  // one a station: station_hydraulic_depth
};
ChannelLine channel_line(const Channel& channel);
// The surface at (x, z) given the stretch it is nearest to, and the search for that stretch over
// stretches [first, last] (clamped to the line's).
ChannelAt channel_at_stretch(const Channel& channel, const ChannelLine& line, u32 stretch, f32 x,
                             f32 z) noexcept;
ChannelAt channel_at(const Channel& channel, const ChannelLine& line, f32 x, f32 z, u32 first = 0,
                     u32 last = 0xffffffffu) noexcept;

// **The sheet** (above; water.md, "The surface from the solve").
struct ChannelMesh {
  Mesh mesh;  // `spacing` the lattice; `edge` and `rings` are a body's and stay 0
  f32 lattice_m = 0.0f;
  u32 triangles = 0;
  u32 fall_triangles = 0;  // triangles with a corner on a fall (UV.y < 0)
  u32 wrap_vertices = 0;   // vertices written a second time for a wrap of the phase
  // The sheet's boundary in plan as segments (pairs of points, metres from the origin, with the
  // level at each): what `channel_edge_exposed` walks.
  Vector<Vec3> boundary;
};
f32 channel_lattice(const Channel& channel) noexcept;
void build_channel_mesh(const Channel& channel, ChannelMesh& out);

// **The depth map** (water.md, "The depth map"): **the level less the ground**, signed as a
// body's (the bank above the level negative, to `k_depth_map_bank_min`), over the sheet's bounding
// square at `k_channel_map_texel_m` on the world's multiples of it. Only the texels within a texel
// of the sheet sample the ground; the rest are bank. Past the wet reach's margin, and on a stretch
// that is not drawn, a texel is never wet. `ground` absent (a scene with no terrain): the bed is
// level at the solved greatest depth across the wet reach, since there is no ground to subtract.
void build_channel_depth_map(const Channel& channel, const ChannelMesh& mesh,
                             const scene_gen::Ground& ground, DepthMap& out);

// **How much of the sheet's edge the ground does not hide**: the boundary's points, sampled every
// `step` metres, that stand more than `clear_m` above the ground — a mesh edge a pixel can show.
// The cuts across the line (a head, a mouth, the end of a run) are not banks and are not counted:
// `cuts` is how many samples lay on one. 0 for a channel whose banks rise at its wet edges.
struct ChannelEdge {
  u32 samples = 0;
  u32 exposed = 0;
  u32 cuts = 0;
  f32 worst_m = 0.0f;  // the highest an exposed sample stands over the ground
};
ChannelEdge channel_edge_exposed(const Channel& channel, const ChannelMesh& mesh,
                                 const scene_gen::Ground& ground, f32 step = 0.05f,
                                 f32 clear_m = 0.001f);

// **A channel's water at a point** (what a walker, a game module's water query or a sound source
// asks; nothing in the engine calls it yet — roadmap A43 points its query here). False where the
// point is not over the channel's water: outside the wet reach, on a stretch that is not drawn, or
// where the ground stands at or above the level. `level_m` is the surface's world height,
// `depth_m` the level less the ground at the point, `velocity` the surface's, m/s, along the
// line's heading there (the section's mean times the surface factor and the lateral share of the
// point's depth; on a fall the nappe's). Linear in the channel's stations: a caller with many
// channels and many points keeps its own index of which channel is near.
struct ChannelSample {
  f64 level_m = 0.0;
  f32 depth_m = 0.0f;
  Vec3 velocity{};
  f32 froude = 0.0f;  // the section's
  bool fall = false;
};
bool channel_sample(const Channel& channel, const ChannelLine& line,
                    const scene_gen::Ground& ground, WorldPos at, ChannelSample& out) noexcept;

// The generator's parameters as the reader hands them: `{"channels": [<WaterChannel>...]}`. Its
// `expand` makes one mesh and one placement per channel, in order.
JsonValue channel_generator_params(std::span<const scene::WaterChannel> channels);

}  // namespace engine::water
