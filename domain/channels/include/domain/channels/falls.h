#pragma once

// **Water that leaves its bed** (roadmap S104, stages 0 and 1; the design and its grounding in
// docs/experiments/waterfalls.md; channels.md, "Where the water leaves its bed (S104)"). Water is
// a projectile its bed may hold up and cannot pull down: the cook marches each column of a lip's
// water down the bed the plan cut, and the column **leaves** where the bed falls away faster than a
// parabola launched along it at the water's speed, **flies** as the free jet's closed form,
// **lands** where that parabola meets the water standing below, a face, or a body's level past a
// mouth, and **slides** where it landed on a face. A flight is a **fall** when it drops at least
// three critical depths and its discharge is a sheet's (a critical depth of a centimetre or more).
//
// What it reads: the plan alone (`ChannelNetwork`: each link's line, its cut bed, its sections),
// never the ground — so the same plan cooks the same bytes wherever it was made. What it writes:
// `FallSet`, a fall a run of the bed's own steps, each with its columns and their paths, as bytes
// beside the plan (`falls_bytes`, `read_falls`, `ensure_falls`) for the water to draw later. It
// moves nothing in the plan: `k_channels_version`, the plan's format and `ChannelLayer` are not
// touched by it, and the falls carry `k_falls_version` of their own.
//
// **Determinism.** Every decision is + - x / and a square root (correctly rounded), the cube root
// a fixed Newton iteration, no C library call; the links are cooked independently and merged by
// link index, so the bytes are the same from every toolchain and at any thread count.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/channels/channels.h>

#include <limits>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::channels {

// Bumped when a change makes an old fall file wrong; in every file's header and its name's key.
// 1 (S104 stages 0 and 1): the fall test and the march (leave, fly, land, slide).
inline constexpr u32 k_falls_version = 1;

// The free jet's constants (docs/experiments/waterfalls.md, "The grounding"): g; the brink depth
// over the critical depth at a free overfall from a mild channel (Rouse 1936, 0.715, so the brink's
// mean speed is v_c / 0.715 = 1.40 sqrt(g d_c)); the test's two thresholds, a flight drop of three
// critical depths (Rand's 1955 fit puts the water standing behind the nappe at under half its drop
// there: y_p / h = (d_c / h)^0.66, 0.48 at three) and a critical depth of a centimetre (3 l/s a
// metre of lip, a choice above the curtain's 0.15 l/s, since a thinner film goes round the lip);
// and the fall per metre of a stretch the cook takes for a face (45 degrees: the gradient at which
// the water capability draws a channel's stretch as falling, `k_channel_fall_gradient`).
inline constexpr f64 k_fall_gravity = 9.81;
inline constexpr f64 k_brink_depth_share = 0.715;
inline constexpr f64 k_fall_min_drop_depths = 3.0;
inline constexpr f64 k_fall_min_critical_m = 0.01;
inline constexpr f64 k_face_gradient = 1.0;
// **The break-up length in critical depths, by the turbulence the water arrives with** (the design
// page's three classes: T_u 0.03 above a pool-riffle, plane-bed or dune-ripple reach, 0.05 above a
// bedrock or colluvial chute, 0.08 above a step-pool or a cascade), from Castillo's form
// L_b / (B F^2) = K / (1.14 T_u F^2)^0.82 at a brink (B = 0.715 d_c, F^2 = 2.74) with **K = 0.77,
// the design page's own fit** to Horeni's 6 q^0.32 at 0.25 m^2/s — not a published constant, and
// neither paper was opened (waterfalls.md, "Break-up"). Carried on each fall for the water's
// coverage (stage 2); nothing in the cook decides by it.
inline constexpr f64 k_breakup_depths[3] = {10.52, 6.93, 4.71};

// The cube root by Newton's iteration from above, a fixed count of + - x /, so the same bits on
// every toolchain (the C library's cbrt is not correctly rounded and differs between them); the
// water capability's `cube_root` is the same iteration.
f64 fall_cube_root(f64 a) noexcept;
// d_c = (q^2 / g)^(1/3) for a discharge q a metre of width, m^2/s.
f64 critical_depth(f64 q) noexcept;

// ---- One column's march (the closed forms; tests hold them to the design page's three scenes)
// ----

// The ground a column runs on, down the line: stations (metres of horizontal run from the line's
// start, ascending), the bed under the column at each, and the depth of water standing over it
// there (0 on a face: a stretch whose bed falls at `k_face_gradient` or more is rock the water
// lands on, not a pool). Between stations the bed is linear, as the cut is. Past the last station
// the landing is a body's level (`tail_body`, a mouth: the sea or a lake standing at
// `tail_level`), else the last station's surface carried on level (the line's end at a junction
// or the region's edge).
struct FallProfile {
  Vector<f64> s;
  Vector<f64> bed;
  Vector<f64> water;
  bool tail_body = false;
  f64 tail_level = 0;
};

enum class FallPhase : u8 { Bed = 0, Air = 1, Slide = 2 };
// Where a flight came down: in water standing over a tread or a pool, on a face (rock), in a body
// past a mouth, or past the line's end.
enum class FallLanding : u8 { None = 0, Water = 1, Rock = 2, Body = 3, End = 4 };

// One flight: where it left and landed (station, height of the lower nappe), how fast and in what
// direction it left (`tan0`: the bed's fall per metre it was launched along, positive downward),
// its time aloft, its speed at landing, and what it landed in.
struct FallFlight {
  f64 s0 = 0, y0 = 0;
  f64 s1 = 0, y1 = 0;
  f64 speed0 = 0, tan0 = 0;
  f64 time_s = 0;
  f64 speed1 = 0;
  FallLanding landing = FallLanding::None;
  f64 drop() const noexcept { return y0 - y1; }
};
// A point of the column's path as the march found it: its station and height, the time since the
// march began, the speed, and the phase it is in.
struct FallSample {
  f64 s = 0, y = 0;
  f64 time_s = 0;
  f64 speed = 0;
  FallPhase phase = FallPhase::Bed;
};
struct FallMarch {
  Vector<FallSample> path;
  Vector<FallFlight> flights;
};
// **The march** of a column of unit discharge `q` (m^2/s), arriving at station `from` at `speed`
// (m/s) along a bed falling `tan_in` per metre (positive downward), down `profile` to station `to`
// (and on past it while a flight is aloft): on the bed its
// speed follows the energy equation against Manning's friction slope (n^2 u^2 / d^(4/3), d = q / u;
// a half-implicit step every 5 cm of bed, which tends to the normal speed and never overshoots
// it); at each station the column **leaves** when the parabola launched along the stretch above,
// at its speed — the brink's, q / (0.715 d_c), where it arrives subcritical — stands more than a
// millimetre over the bed at the next station; aloft it is the free jet, y = y0 - tan0 x - g x^2
// (1 + tan0^2) / (2 v^2), its landing the first root against each stretch's surface in turn (a
// quadratic, solved in the stable form); landed, it keeps the part of its velocity along the
// surface it struck. False for a profile whose arrays disagree or a q or speed that is not
// positive.
bool march_column(const FallProfile& profile, f64 q, f64 speed, f64 manning_n, u32 from, u32 to,
                  f64 tan_in, FallMarch& out);

// ---- The cook: every run of the bed's own steps in a plan -------------------------------------

// What a run came to (the test, stage 0): the bed held the water (no column left it), it left and
// no column flew three critical depths (a drowned step: S103's hole), the discharge is under a
// sheet's (wet rock), or a fall: a plunge (it lands in water), a horsetail (it lands on a face
// and slides), tiered (a column makes two falls or more within the run).
enum class FallKind : u8 {
  Held = 0,
  Drowned = 1,
  Trickle = 2,
  Plunge = 3,
  Horsetail = 4,
  Tiered = 5
};

// The cook's records, written to the file as their bytes (no padding; channels.md, "Where the water
// leaves its bed (S104)", has the layout). Positions are whole millimetres from the fall's origin.
//
// A vertex of a column's path: its place (x, y, z, mm from the fall's origin), its travel time
// from the column's first vertex (microseconds), its speed (mm/s), the water's thickness there (q
// over the speed, micrometres) and its phase (`FallPhase`).
struct FallVertex {
  i32 x_mm = 0, y_mm = 0, z_mm = 0;
  u32 time_us = 0;
  u32 speed_mm_s = 0;
  u32 thickness_um = 0;
  u8 phase = 0;
  u8 pad[3] = {};
};
// A column of a lip: its place across the line (mm, positive to the left, toward (-dz, dx)), its
// unit discharge (mm^2/s, a metre of lip) and critical depth (um), the drop of its highest flight
// (mm; 0: it never left), its path (`first_vertex`, `vertex_count` into `FallSet::vertices`), its
// kind (`FallKind`), where its highest flight landed (`FallLanding`) and its flights.
struct FallColumn {
  i32 offset_mm = 0;
  u32 q_mm2_s = 0;
  u32 critical_um = 0;
  i32 drop_mm = 0;
  u32 first_vertex = 0;
  u32 vertex_count = 0;
  u8 kind = 0;
  u8 landing = 0;
  u8 flights = 0;
  u8 pad = 0;
};
// A fall: one run of a link's stretches whose bed falls at 45 degrees or more (the solver's own
// steps), tested whatever it came to. Its origin is the line's point at the run's first point (the
// lip) at the bed's height, world mm; the link, the lip and the run's last point; the line's
// heading at the lip (ten-thousandths of a unit vector); the discharge (ml/s); the critical depth
// at the lip's mean unit discharge (um); the centre column's launch speed (mm/s); the highest
// column drop (mm); the break-up length by the reach's class (mm, `k_breakup_depths`: K = 0.77 is
// the design page's fit); where the deciding flight landed, relative to the origin's height (mm),
// and the water's depth there (mm, 0 on rock or past the line's end); its columns; its kind and
// landing; the reach above the lip and its turbulence class (0 to 2); whether the run lies in a
// marked knickpoint (`ChannelLink::knick`); and whether the water's description draws the link
// (its last kept point drains at least `FallOptions::min_area_m2`).
struct Fall {
  i64 x_mm = 0, y_mm = 0, z_mm = 0;
  u32 link = 0, lip = 0, end = 0;
  i32 heading_x = 0, heading_z = 0;
  u32 discharge_ml_s = 0;
  u32 critical_um = 0;
  u32 launch_mm_s = 0;
  i32 drop_mm = 0;
  u32 breakup_mm = 0;
  i32 tail_level_mm = 0;
  u32 tail_depth_mm = 0;
  u32 first_column = 0, column_count = 0;
  u8 kind = 0;
  u8 landing = 0;
  u8 reach = 0;
  u8 turbulence = 0;
  u8 knick = 0;
  u8 drawn = 0;
  u8 pad[2] = {};
};

// The count (stage 0's answer): the links tested (every link of two kept points or more, whether
// or not the water's description draws it: a link that drains under `min_area_m2` still has its
// steps, and a mouth's last points over a sea cliff are a link's too), the runs of steps found,
// what each came to, the falls on links the description draws and at mouths, the highest drop
// and the cook's columns and vertices.
struct FallStats {
  u64 links = 0;
  u64 runs = 0;
  u64 held = 0;
  u64 drowned = 0;
  u64 trickles = 0;
  u64 falls = 0;
  u64 plunges = 0;
  u64 horsetails = 0;
  u64 tiered = 0;
  u64 drawn_falls = 0;
  u64 mouth_falls = 0;
  u64 knick_falls = 0;
  u64 columns = 0;
  u64 vertices = 0;
  f64 highest_m = 0;
};

// The cook's choices: the water's depth at a lip is `stage` of the section's thalweg (as
// `water_channels_json` lays it in the cut), its speed Manning's at `manning_n` (the description's
// default, which the water draws with) over the bed's fall above the lip; `columns` across the
// water's width at the lip (3 to 33); `min_area_m2` the description's threshold, for `drawn`.
struct FallOptions {
  f32 stage = .5f;
  f32 manning_n = .04f;
  u32 columns = 9;
  f64 min_area_m2 = 50000;
};
bool fall_options_in_range(const FallOptions& options, std::string& error);
u64 fall_options_hash(const FallOptions& options);

struct FallSet {
  u64 plan_key = 0;  // the plan it was cooked from (0 for a network that was not read as a plan)
  u64 options = 0;   // `fall_options_hash`
  Vector<Fall> falls;
  Vector<FallColumn> columns;
  Vector<FallVertex> vertices;
  FallStats stats;
};
// **The cook**: every run of every link of `network`, in link order and down each link, whatever
// it comes to. `jobs` (nullable) cooks the links a job each; the result does not depend on it.
bool cook_falls(const ChannelLayer& layer, const ChannelNetwork& network, u64 plan_key,
                const FallOptions& options, jobs::JobSystem* jobs, FallSet& out,
                std::string& error);

// The file: a header (magic, `k_falls_version`, the plan's key, the options' hash, the counts),
// the stats, the three record arrays as their bytes, little-endian, closed by a hash of the whole.
// `read_falls` is **the reader**: it refuses a file that is short, of another version, changed,
// or whose indices run past their arrays.
std::string falls_bytes(const FallSet& set);
bool read_falls(std::string_view bytes, FallSet& out, std::string& error);
// "<dir>/falls-<plan key>-<options hash>.channel-falls", beside the plan in `derived_dir`'s.
std::string falls_path(std::string_view dir, u64 plan_key, u64 options);
// Reads the plan's falls under `dir` when they are there and are its own, else cooks and writes
// them (best effort, as the plan is). `cooked` says which.
bool ensure_falls(const ChannelLayer& layer, const ChannelPlan& plan, const FallOptions& options,
                  jobs::JobSystem* jobs, std::string_view dir, FallSet& out, bool& cooked,
                  std::string& error);
// The falls as JSON, for a person or a test to read: each fall's place, its numbers in metres and
// seconds, its kind, and each column's path as [x, y, z, t, speed, thickness, phase] rows.
JsonValue falls_json(const ChannelLayer& layer, const FallSet& set, bool paths);

}  // namespace engine::channels
