#pragma once

// **A stream's surface, solved in its own bed** (roadmap W120 stage B; channels.md, "The
// profile"; the grounding in docs/experiments/stream-profile.md): steady, gradually varied flow in
// one dimension down a line, through the sections the ground itself has across that line.
//
// **What it takes: a line, a discharge and a ground.** Nothing of the channels planner: the
// planner is one writer of lines (`flow_lines`, discharge.h), and a line an author wrote on an
// imported terrain is solved by the same call. The ground is a `scene_gen::Ground` — a function
// pointer and a context — so this module names no renderer and no tile set; the cook hands it the
// composed ground the scene draws.
//
// **What it does.** The line is walked in nodes one lattice apart. At each node the ground is
// sampled across the line, and the section is whatever those heights are: its area, wetted
// perimeter, top width and first moment against a level come from the heights alone, counting only
// ground connected to the thalweg under the level and stopping at each bank's crest. From them:
// critical depth (Q^2 T = g A^3); a resistance (Jarrett 1984 by the bed's own slope, or one
// Manning's n); the standard step method upstream from every control in tranquil flow and
// downstream in shooting flow; the regime where both stand by specific force, as a jump placed
// where the shooting flow's sequent level is the level the tranquil profile holds a jump's length
// on; and, where the bed drops three critical depths or more (S104's fall test at the solved
// critical depth), a fall: critical at the lip, Rand's (1955) drop at the foot, and no surface
// between. The description keeps the fewest nodes that hold the pins (stations.h).
//
// **Determinism.** + - x /, the square root, the falls' cube root and `core/math/deterministic.h`'s
// exp and log for the two powers; bisections run until their bracket is one float wide; a line is
// solved from its own inputs alone, so lines may be solved on any thread in any order.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/water_channel.h>

#include <schemas/scene.h>
#include <string>

namespace engine::channels {

// **The resistance of a steep bed** (Jarrett 1984, in metres): n = 0.32 S^0.38 R^-0.16, S the
// bed's fall over the reach about the node and R the hydraulic radius at the depth in hand, each
// held to the range of his measurements (above a slope of 0.04 one of the two quotations of that
// range has none: an extrapolation, stream-profile.md).
inline constexpr f64 k_jarrett_n = 0.32;
inline constexpr f64 k_jarrett_slope_exponent = 0.38;
inline constexpr f64 k_jarrett_radius_exponent = -0.16;
inline constexpr f64 k_jarrett_slope_min = 0.002;
inline constexpr f64 k_jarrett_slope_max = 0.09;
inline constexpr f64 k_jarrett_radius_min_m = 0.15;
inline constexpr f64 k_jarrett_radius_max_m = 2.1;
// The reach the bed's slope is taken over, each way from the node, metres (inside one stretch
// between falls: a fall's drop is not friction's).
inline constexpr f64 k_reach_slope_m = 10.0;
// A bank's crest is where the ground, walking out from the thalweg, stops rising and falls this
// far below the highest it reached: past it the ground is another hollow's.
inline constexpr f64 k_bank_crest_drop_m = 0.02;
// ... and a quarter of the height it has climbed over the thalweg: a ledge on a high bank's face
// is not the bank's top (on the forest's ground a 2 cm dip in a trench's wall read as its crest,
// and a pool standing in the trench as 72 m over its bank).
inline constexpr f64 k_bank_crest_drop_share = 0.25;
// A level stands over a bank when it is this far over the crest.
inline constexpr f64 k_over_bank_m = 0.001;

enum class Resistance : u8 { Jarrett = 0, Manning = 1 };

struct ProfileOptions {
  // The ground's lattice, metres: the nodes' spacing along the line (never less; a line's length
  // is divided into whole steps at least this long) and the samples' spacing across it.
  f64 lattice_m = 0.25;
  Resistance resistance = Resistance::Jarrett;
  f64 manning_n = 0.04;  // with `Resistance::Manning`
  // How far across the line a section is read each side where the line names no width, metres.
  f64 reach_m = 12.0;
};

// A line to solve: its points downstream (y is not read), the discharge down it (one entry for
// the whole line or one a point, m^3/s), and optionally the bed's bankfull width at each point
// (which bounds where the thalweg is looked for and how far a section is read; empty: the
// thalweg is the nearest hollow to the line and the section reaches `reach_m`).
struct FlowLine {
  std::string name;
  Vector<WorldPos> points;
  Vector<f64> discharge;
  Vector<f64> width;
  // What the line ends in: a level it meets (a body's at a mouth, the main stem's at a
  // confluence), or nothing (the region's edge: normal depth).
  bool tail_level = false;
  f64 level = 0;
  scene_gen::ChannelControl tail = scene_gen::ChannelControl::Mouth;
  // The line begins where a body spills (a lake's outlet): its first station is that control.
  bool head_spill = false;
};

// A station of the solved description, as `WaterChannel` carries it (level and place to the
// millimetre), with what the cook reports beside it.
struct ProfileStation {
  f64 s = 0;  // metres along the line
  f64 x = 0, z = 0;
  f64 level = 0;
  f64 bed = 0;  // the thalweg's ground
  f64 depth = 0;
  f64 friction = 0;
  f64 discharge = 0;
  f64 speed = 0;
  f64 froude = 0;
  f64 wet_left = 0, wet_right = 0;
  f64 width = 0;      // the line's width there, else the section's from crest to crest
  f64 manning_n = 0;  // the n the station was solved with
  f64 over_m = 0;     // how far the solved level stood over the lower bank (0: it did not)
  u16 flags = 0;
};

// What the solve came to. A **rise** is a station whose level stands more than half a millimetre
// over the station upstream of it: a jump's (the pair), a shooting run's (water that shoots and
// is slowed thickens, so its level may climb: physical), one beside a station over its bank (the
// level written there is the bank's), and any other, which there should be none of. The total
// head, level plus V^2 / 2g, never rises downstream; `head_rises` counts where it does.
struct ProfileStats {
  u32 nodes = 0;
  u32 stations = 0;
  f64 length_m = 0;
  u32 crests = 0;  // critical controls that are not a fall's lip
  u32 falls = 0;
  u32 jumps = 0;
  u32 drowned_feet = 0;  // falls whose foot stands in tranquil water (a plunge pool)
  // The level the line ends at stands under its bed there (a tributary hanging over its main
  // stem): the last station is the line's own and not that level.
  bool tail_hung = false;
  u32 over_bank = 0;
  f64 over_bank_worst_m = 0;
  WorldPos over_bank_at{};
  u32 rises_jump = 0;
  u32 rises_shooting = 0;
  u32 rises_over_bank = 0;
  u32 rises_other = 0;
  f64 rise_other_worst_m = 0;
  WorldPos rise_other_at{};
  f64 rise_shooting_worst_m = 0;
  WorldPos rise_shooting_at{};
  f64 rise_jump_worst_m = 0;
  u32 head_rises = 0;
  f64 head_rise_worst_m = 0;
  f64 highest_fall_m = 0;
  f64 manning_n_mean = 0;  // by length
  f64 froude_max = 0;
};

struct Profile {
  Vector<ProfileStation> stations;
  ProfileStats stats;
};

// **The solve.** False with a sentence for a line of fewer than two points, a point or a
// discharge that is not a positive finite number, arrays of the wrong length, an absent ground or
// options out of range (a lattice of 1 cm to 100 m, an n of 0.005 to 1).
bool solve_profile(const FlowLine& line, const scene_gen::Ground& ground,
                   const ProfileOptions& options, Profile& out, std::string& error);

// The profile as the description's block: `points`, `width_m`, `depth_m`, `slope`,
// `discharge_m3s`, `speed_mps`, `froude`, `wet_left_m`, `wet_right_m`, `flags`, `manning_n` (the
// stations' mean by length) and the name and seed given; the optics are the schema's defaults.
scene::WaterChannel describe_profile(const std::string& name, const Profile& profile, u64 seed);

}  // namespace engine::channels
