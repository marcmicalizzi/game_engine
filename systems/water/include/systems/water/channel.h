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
#include <domain/scene_gen/water_channel.h>
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
// solved description (S106). 3: the sheets cut where they meet, a pool over its bank drawn, a
// nappe as wide as its lip and thinning to its sides (S108). 4 was taken twice, on two branches: a
// reach vertex's UV.y carries the section's Froude number in its low bits (W139,
// `pack_channel_hydraulic`), and no sheet climbs from or to a station over its bank (W138). 5:
// both, assembled on one branch (b67-streams). 6: a vertex past the section's conveying reach
// carries no Froude number and no slope (W144, `channel_carry`).
inline constexpr u32 k_channel_mesh_version = 6;
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
// **Broken water on a steep bed** (W130; water.slang's `water_channel_broken_share`, which this
// mirrors; steep-streams.md). The share of a channel's surface drawn white where its sheet falls
// `slope` (rise over run) at a section speed `speed` over `depth` of water: rising over the slope
// from the step-pool reach's onset to the cascade's upper end (Montgomery and Buffington 1997), to
// `k_channel_steep_white` (a choice), times the energy to entrain air (g S V over the lower end of
// Ulseth et al. 2019's bubbly regime and ten times it) and times the water (a nappe's veil depths,
// 1.5 mm to 1 cm). A level pool, a gentle reach and a film have none.
inline constexpr f64 k_channel_steep_from = 0.03;
inline constexpr f64 k_channel_steep_full = 0.30;
inline constexpr f64 k_channel_steep_white = 0.6;
inline constexpr f64 k_channel_dissipation_from = 0.02;
inline constexpr f64 k_channel_dissipation_full = 0.2;
inline constexpr f64 k_channel_veil_from_m = 0.0015;
inline constexpr f64 k_channel_veil_full_m = 0.010;
f64 channel_broken_share(f64 slope, f64 speed, f64 depth_m) noexcept;
// **White needs a section of water, not a film** (W139; need 140): a sheet of millimetres over
// gravel runs as threads between the clasts and has no water to entrain air into, however steep or
// fast it is, so neither rule whitens a section whose hydraulic depth is under the first, and both
// are whole by the second (a choice: the clasts of a small steep bed stand a few centimetres
// proud; steep-streams.md, "The white's inputs per vertex"). W120's films are 5 mm, which until
// W139 drew white blobs on the dry-looking gravel beside a pool (the forest's v48 mark 14).
inline constexpr f64 k_channel_white_depth_from_m = 0.01;
inline constexpr f64 k_channel_white_depth_full_m = 0.03;
f64 channel_white_depth_gate(f64 hydraulic_m) noexcept;

// **The section's Froude number rides on each vertex** (W139; needs 128, 139). The draw used to
// take a triangle's speed from its phase gradient, one speed a triangle: the white was faceted
// along every edge (128), and on the outer side of a bend, where every lattice node abeam the
// station has the station's own phase, a triangle with a corner just past that wedge read a speed
// of the cell over a phase difference near nothing — tens of metres a second in a pool of 0.1, a
// Froude number past every bound, one pure white triangle (139). So a reach vertex carries the
// section's Froude number in the lowest `k_channel_froude_bits` of its UV.y's half float (the
// hydraulic depth, rounded to the half's other bits: within 1.6 %), the resolve decodes each corner
// and interpolates both, and the speed is Fr sqrt(g D) at the pixel: one smooth field, and never
// past the station's. The number is stored as q = round(31 sqrt(Fr / k_channel_froude_max)), so a
// pool's 0.02 to 0.22 keeps its steps of under 0.02 and a torrent's 3 of 0.2. A nappe's vertex
// (UV.y negative, its thickness) carries nothing. water.slang's `water_channel_unpack` is the
// inverse; a value that is not a reach's (negative, or zero) packs as itself.
inline constexpr u32 k_channel_froude_bits = 5;
f32 pack_channel_hydraulic(f32 hydraulic_m, f32 froude) noexcept;
void unpack_channel_hydraulic(f32 packed, f32& hydraulic_m, f32& froude) noexcept;
// **What the surface's pattern travels at** (W137; water.slang's `water_channel_travel_speed`,
// which this mirrors; docs/experiments/stream-speed.md, "What the eye reads: the threads, not the
// mean"). A steep stream's solved speed is a mean over its steps and pools; what an eye follows on
// it is the jets over each step and the white they carry, which have fallen a step's height:
// sqrt(v^2 + 2 g H). The pattern on a sheet falling `slope` is carried at sqrt(surface^2 + 2 g H
// w), H `k_channel_jet_m` (a choice, the clasts' scale the broken water is drawn on) and w the
// step-pool's weight the broken share rises over; a level pool and a reach under 3 % travel at
// their surface's speed. Only the pattern's travel reads it: the depth, the Froude number and every
// rule on them keep the solve's own speed.
inline constexpr f64 k_channel_jet_m = 0.25;
f64 channel_travel_speed(f64 surface_speed, f64 slope) noexcept;
// **A station's numbers describe the water that carries its discharge, not all the water at its
// level** (W144; need 149's blotches on a slow pool; steep-streams.md, "The white's inputs per
// vertex"). A station's speed, Froude number and fall are its conveying section's: a top width of
// T = Q / (V D), which with D = V^2 / (g Fr^2) is Q g Fr^2 / V^3. Where a stream runs into a pool
// the solve writes the station at the pool's level (W138) and its wet reach is where that level
// meets the ground, the pool's own shore, ten metres off on the forest where T is under one: the
// water out there stands. Until W144 every vertex of the reach carried the station's Froude number
// and the stretch's fall, so a pool beside an inflow falling 12 % at Froude 0.44 was drawn with a
// step-pool reach's white scattered over it (the forest's v50 mark 14; 791 such stations of its
// 112,906 can whiten).
//
// So each side of a station has a **conveying reach**: its wet reach, or where that is wider, the
// larger of the bed's own width (`width_m`: water within the bed's width of the line is the
// channel's whatever the numbers say, which keeps a fall's foot written at a bank's top whole) and
// what the top width leaves of `k_channel_convey_slack` T after the other side's reach (the slack
// for a description's rounding: 96 % of the forest's stations have a wet reach within a quarter of
// T). A vertex's **carry** is 1 wherever the section's conveying reach is its whole wet reach, and
// where it is less, 1 within the conveying reach, the margin under the bank and a lattice cell
// past it, falling to 0 over one more conveying reach (a choice: an inflow's jet spreads into
// standing water over about its own width); the Froude number and the fall a vertex carries are
// the section's times it. **A channel whose stations convey over their whole wet reach is written
// to the same bytes as before the rule** (measured, 2026-11-18, each channel built with the rule
// and without: the test fixtures, the torrent judging scene's eight channels and the forest's
// 542, 1,973,721 vertices; by distance alone 311 vertices of 95 such channels moved, places the
// sheet keeps past a section's own reach on the outer side of a bend whose stations' reaches
// differ, which is why the rule asks first whether the section is narrowed at all). An authored
// line gets it from its own numbers: one whose discharge, speed and Froude number agree with its
// wet reach is untouched however wide it is.
inline constexpr f64 k_channel_convey_slack = 1.25;
// The top width a station's own numbers say, metres: Q g Fr^2 / V^3; where the speed or the Froude
// number is 0 the numbers say nothing and the width is unbounded (1e30).
f64 channel_convey_width(f64 discharge_m3s, f64 speed_mps, f64 froude) noexcept;
// One side's conveying reach, metres: `wet_m` that side's wet reach, `other_wet_m` the other's,
// `bed_width_m` the description's `width_m`, `top_m` `channel_convey_width`'s.
f64 channel_convey_reach(f64 wet_m, f64 other_wet_m, f64 bed_width_m, f64 top_m) noexcept;
// The carry at `distance_m` from the line where the side's conveying reach is `convey_m` and the
// margin under the bank `margin_m`.
f64 channel_carry(f64 distance_m, f64 convey_m, f64 margin_m) noexcept;

// One channel as this capability draws it: the solved description about its origin, bounded.
struct Channel {
  std::string name;
  // Where the instance stands: the centre of the points' bounding box in x and z taken down to a
  // whole metre (so the sheet's lattice and the depth map's texels are the world's multiples of
  // their spacing), and the first point's level (ADR-0053: the mesh is metres from here).
  WorldPos origin{};
  // The stations less the origin, metres: y the solved level.
  Vector<Vec3> points;
  Vector<f32> depth_m;    // the solved greatest depth
  Vector<f32> speed_mps;  // the section's mean, within [k_channel_min_speed, k_channel_max_speed]
  Vector<f32> froude;     // within [0, k_channel_froude_max]
  Vector<f32> discharge_m3s;
  Vector<f32> wet_left_m;  // left is the side of (-dz, dx) for a line running (dx, dz)
  Vector<f32> wet_right_m;
  // Each side's conveying reach (W144; `channel_convey_reach`): at most the wet reach. Empty (a
  // channel not read from a description) is the wet reach.
  Vector<f32> convey_left_m;
  Vector<f32> convey_right_m;
  Vector<u16> flags;  // scene_gen/water_channel.h
  f32 manning_n = 0.04f;
  f32 shore_m = 2.0f;
  u64 seed = 1;
  // **An end that meets other water is round, not square** (S108; `channel_joins`): where the
  // channel's head or its last station stands in another channel's water, the sheet is not cut
  // across the line there but runs on as far past the station as it is wide, level at the
  // station's level, so no wedge of dry bed is left between two sheets whose headings differ. Set
  // by whoever knows the other channels (the generator); a channel alone is cut square.
  bool cap_head = false;
  bool cap_tail = false;
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
// **Drawn unless both its stations stand over a bank and the water written there is a film or
// climbs** (S108). W120 flags a station whose solved level would stand over its lower bank and
// writes it at that bank's crest. Two kinds of stretch carry the flag at both ends. Along a line
// that climbs out of a hollow the ground does not fill, the cook writes a 5 mm film at the bed
// rising to the spill: drawing that is water going uphill, and it is not drawn. Below a spill it
// writes the pool the bed holds, level at the spill's height with centimetres to decimetres of
// water (48 m of it below the judging scene's rock step): that water is there, brim-full to its
// lower bank, and leaving it out is a bed with no water. So such a stretch is drawn when both
// stations hold at least `k_channel_over_bank_depth_m` and the level does not rise along it by
// more than `k_channel_over_bank_rise_m`. (Until S108 no stretch flagged at both ends was drawn.)
// **Nor is a stretch with one end over a bank whose level rises** (W138): the level written at
// that end is the bank's top, so a sheet climbing from it to a pool's level (0.5 to 3.1 m over a
// quarter metre on the forest) is a face of water with nothing under it.
// The rule is `scene_gen::channel_stretch_drawn`, which the cook's count of runs reads too.
inline constexpr f32 k_channel_over_bank_depth_m = scene_gen::k_channel_over_bank_depth_m;
inline constexpr f64 k_channel_over_bank_rise_m = scene_gen::k_channel_over_bank_rise_m;
inline bool over_bank_stretch_drawn(f32 depth_a, f32 depth_b, f64 level_a, f64 level_b) noexcept {
  return scene_gen::channel_stretch_drawn(scene_gen::k_channel_over_bank,
                                          scene_gen::k_channel_over_bank, depth_a, depth_b, level_a,
                                          level_b);
}
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
  f32 wet_inside = -1.0f;  // the same without the margin: > 0 within the wet reach itself
  // The terms of it, which the sheet's pieces take their own least of: how far along the line the
  // place is (past the line's own ends for a place beyond them), how far inside the wet reach,
  // and the margin under the bank there (0 at a station over its bank, and on a nappe).
  f32 along = 0.0f;
  f32 lateral = -1.0f;
  f32 margin = 0.0f;
  f32 level = 0.0f;  // metres over the origin's y; on a fall the nappe's parabola
  f32 slope = 0.0f;  // d level / d metre along the heading
  // The share of the section's Froude number and fall the place's water has (W144,
  // `channel_carry`): 1 in the conveying reach, 0 in water that stands at the station's level.
  f32 carry = 1.0f;
  Vec2 heading{1.0f, 0.0f};  // the stretch's direction in plan, unit, downstream
  f32 speed = 0.0f;          // the section's mean, m/s; on a fall the nappe's own
  f64 tau = 0.0;             // the travel time from the head, seconds, unwrapped
  // The section's hydraulic depth, metres; on a fall less the nappe's thickness (negative).
  f32 hydraulic = 0.0f;
  f32 depth_m = 0.0f;  // the solved greatest depth at the place along the line
  bool fall = false;
};
// The line's per-stretch constants, computed once: where each station is along the line in plan
// and in travel time, each stretch's heading, and the runs of drawn stretches.
struct ChannelLine {
  Vector<f32> station;   // one a station, metres along the line in plan
  Vector<f64> tau;       // one a station, seconds from the head
  Vector<Vec2> heading;  // one a stretch
  Vector<f32> length;    // one a stretch, in plan
  Vector<f32> run_from;  // one a stretch: the station its run of drawn stretches starts at
  Vector<f32> run_to;    // ... and ends at (both 0 for a stretch that is not drawn)
  // One a stretch: the stations its **piece** starts and ends at — its run cut at every fall's lip
  // and foot, so a nappe is a piece of its own and a lip is an edge of the mesh.
  Vector<f32> piece_from;
  Vector<f32> piece_to;
  Vector<f32> hydraulic;  // one a station: station_hydraulic_depth
  // The stations' bounds in plan, metres from the origin, and the widest wet reach of any: what a
  // search for a channel near a place rejects by (S108).
  Vec2 lo{};
  Vec2 hi{};
  f32 widest = 0.0f;
};
ChannelLine channel_line(const Channel& channel);
// The surface at (x, z) given the stretch it is nearest to, and the search for that stretch over
// stretches [first, last] (clamped to the line's).
ChannelAt channel_at_stretch(const Channel& channel, const ChannelLine& line, u32 stretch, f32 x,
                             f32 z) noexcept;
ChannelAt channel_at(const Channel& channel, const ChannelLine& line, f32 x, f32 z, u32 first = 0,
                     u32 last = 0xffffffffu) noexcept;

// **Where channels meet** (S108; water.md, "Where channels meet"). A network's links are separate
// descriptions that share a station: two end at a confluence and a third begins there, each
// carrying the level of the water it meets. Drawn each by itself they overlap as plates (a
// tributary's last stretch lies across the stream it joins, at its own level) and leave wedges of
// dry bed where their square ends turn. So **a link's water ends where the water it joins
// begins, and meets it at its level**: of the channels that meet at a station, the one carrying
// more water there (on a tie the one that begins there, then the lower index) keeps its sheet
// whole; each other is cut where it enters that one's wet reach — less one cell of its own
// lattice, so the two lap and no lattice leaves a gap — and its level comes down to the joined
// water's over the last
// `k_channel_join_blend_m` before it, ending `k_channel_join_under_m` under it, so the two
// surfaces cross without a step and the joined water lies on top where both are.
inline constexpr f32 k_channel_join_blend_m = 1.5f;
inline constexpr f32 k_channel_join_under_m = 0.03f;
// How near an end stands to another channel's wet reach and still meets it, metres.
inline constexpr f32 k_channel_join_reach_m = 0.5f;
struct ChannelJoin {
  const Channel* other = nullptr;     // the water joined (it outranks this channel here)
  const ChannelLine* line = nullptr;  // its line
  u32 first = 0;                      // its stretches within the meeting's reach
  u32 last = 0;
  Vec2 centre{};           // the meeting, metres from this channel's origin
  f32 radius = 0.0f;       // past it no place of this channel is cut or lowered
  Vec3 shift{};            // this channel's origin less the other's, metres
  bool from_left = false;  // the side of the other's line this channel arrives from
};
struct ChannelJoins {
  Vector<ChannelJoin> list;
};
// The joins of channel `k` among `channels` (their lines in `lines`, one each), and whether its
// two ends are round: `cap_head` and `cap_tail` for `channels[k]`.
ChannelJoins channel_joins(std::span<const Channel> channels, std::span<const ChannelLine> lines,
                           u32 k, bool& cap_head, bool& cap_tail);
// A place of the channel against its joins: `outside`, how far the place is from the nearest
// joined water's wet reach (negative inside it; the sheet is kept where it is more than less one
// lattice), and the level there, brought down to the joined water's near it. With no join in
// reach: a large `outside` and `level` as given.
struct ChannelMeet {
  f32 outside = 1e30f;
  f32 level = 0.0f;
};
ChannelMeet channel_meet(const ChannelJoins& joins, f32 x, f32 z, f32 level) noexcept;

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
// `joins`: the waters the channel meets (`channel_joins`); none for a channel alone.
void build_channel_mesh(const Channel& channel, ChannelMesh& out,
                        const ChannelJoins* joins = nullptr);

// **The depth map** (water.md, "The depth map"): **the level less the ground**, signed as a
// body's (the bank above the level negative, to `k_depth_map_bank_min`), over the sheet's bounding
// square at `k_channel_map_texel_m` on the world's multiples of it. Only the texels within a texel
// of the sheet sample the ground; the rest are bank. Past the wet reach's margin, and on a stretch
// that is not drawn, a texel is never wet. `ground` absent (a scene with no terrain): the bed is
// level at the solved greatest depth across the wet reach, since there is no ground to subtract.
void build_channel_depth_map(const Channel& channel, const ChannelMesh& mesh,
                             const scene_gen::Ground& ground, DepthMap& out,
                             const ChannelJoins* joins = nullptr);

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
// asks; a game module's `water_at` asks it through `water::WaterQuery`, query.h). False where the
// point is not over the channel's water: outside the wet reach, on a stretch that is not drawn, or
// where the ground stands at or above the level. `level_m` is the surface's world height,
// `depth_m` the level less the ground at the point, `velocity` the surface's, m/s, along the
// line's heading there (the section's mean times the surface factor and the lateral share of the
// point's depth; on a fall the nappe's). Linear in the channel's stations: a caller with many
// channels and many points keeps its own index of which channel is near, and may name the
// stretches [first, last] the nearest place is looked for among (`channel_at`'s), which answers
// as the whole line does when they hold every stretch within the channel's reach of the point
// (`WaterQuery`'s index, query.h).
struct ChannelSample {
  f64 level_m = 0.0;
  f32 depth_m = 0.0f;
  Vec3 velocity{};
  f32 froude = 0.0f;  // the section's
  bool fall = false;
};
bool channel_sample(const Channel& channel, const ChannelLine& line,
                    const scene_gen::Ground& ground, WorldPos at, ChannelSample& out, u32 first = 0,
                    u32 last = 0xffffffffu) noexcept;

// The generator's parameters as the reader hands them: `{"channels": [<WaterChannel>...]}`. Its
// `expand` makes one mesh and one placement per channel, in order.
JsonValue channel_generator_params(std::span<const scene::WaterChannel> channels);

}  // namespace engine::water
