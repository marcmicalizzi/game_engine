#pragma once

// landform capability (ADR-0027; docs/subsystems/landform.md, docs/experiments/landform.md;
// roadmap W95, stage A of G36's erosion and hydrology).
//
// An offline solver: a region sketch (the lattice, what each edge is, outlets, uplift, how hard the
// rock is, a seed, the time and the step) becomes an eroded ground — an authored heightfield the
// tile set builds as it is (`engine.terrain.heightfield/1`, whole millimetres) and, on the same
// lattice, the fields the later solvers read: drainage area, receiver, slope, erosion rate, fill
// and sediment. The model is the published one: Priority-Flood routing (Barnes, Lehman and Mulla
// 2014), the stream-power law solved implicitly along the flow tree (Braun and Willett 2013) at n =
// 1 and m = 1/2, and hillslope diffusion with a critical slope (Roering, Kirchner and Dietrich
// 1999), with the sediment the rivers carry laid down again (stage B: the erosion and deposition
// law of Davy and Lague 2009 in Yuan et al. 2019's implicit form; landform.md, "Deposition").
// It owns no runtime state: nothing ticks, nothing is drawn, nothing is kept resident.
//
// The same bytes from every toolchain and at any thread count: every decision is IEEE `+ - * /`,
// one correctly rounded square root and integers, the flood's order is a total order on
// (height, cell index), and the job system runs only the loops whose points do not read each
// other's results (landform.md, "The same bytes everywhere").
//
// Additive by construction (ADR-0027 decision 2): ENGINE_WITH_LANDFORM=OFF or ENGINE_MINIMAL=ON
// removes the module, its schema types, its tests and its bench, and `engine-content landform`
// then says the capability is absent.
//
//   [x] schema types      schemas/landform.schema (the sketch, the field file's header)
//   [-] scheduler entry   none: an offline solver has no tick
//   [-] render passes     none
//   [-] derived data      not yet a content-build step; `engine-content landform` is its host
//   [-] protocol methods  none
//   [-] tunables          none: the sketch carries every parameter, so a sketch is its result
//   [-] LOD policy        none of its own: the tile set's far chain filters what it writes
//   [x] determinism       k_determinism below
//   [x] zero cost unused  nothing is registered and nothing runs unless a host calls solve()
//   [x] docs, tests, size table, bench

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/landform.h>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::landform {

/// "derived": the solver's output is content, built ahead and read by the tile set; it is never
/// simulation state. Its bytes are nonetheless pinned across toolchains and thread counts.
inline constexpr const char* k_determinism = "derived";

/// Bumped whenever the same sketch would solve to different bytes; the field file records it.
inline constexpr u32 k_solver_version = 10;

inline constexpr std::string_view k_fields_format = "engine.landform.fields";
inline constexpr u32 k_fields_version = 5;

/// The wave base a sea takes when its sketch names none and has no wave climate to compute
/// Hallermeier's outer limit from, metres (authored: docs/experiments/coast.md, "The depth of
/// closure, and the wave base").
inline constexpr f64 k_default_wave_base_m = 20.0;

/// `substrate`'s values (docs/subsystems/landform.md, "The field file").
enum class Substrate : u8 {
  None = 0,
  Sand = 1,
  Shingle = 2,
  Boulders = 3,
  Rock = 4,
  Cliff = 5,
  Mud = 6
};

// ---- the coast's closed forms (src/coast.cpp; docs/experiments/coast.md) ------------------------
// Pure functions of their arguments, with no C library call: the fractional powers are
// `det::exp` and `det::log`. The stand reads each once per wave entry or per shore segment.

/// Dean's profile scale A, m^(1/3), for a median grain `grain_mm`: 0.21 D^0.48 (Dean 1991).
f64 profile_scale(f64 grain_mm) noexcept;
/// The beach face's slope, tan beta, for a median grain: 0.268 - 0.154 (D - 0.125)^-0.145 held to
/// [0.01, 0.2] (the form of Bujan, Cox and Masselink 2019; its coefficients authored, said so on
/// the page).
f64 face_slope(f64 grain_mm) noexcept;
/// Hallermeier's inner closure depth from the height exceeded twelve hours a year and its period,
/// metres: 2.28 H - 68.5 H^2 / (g T^2).
f64 closure_depth(f64 storm_height_m, f64 period_s) noexcept;
/// Hallermeier's outer limit, past which the year's waves move no sand of the grain: 0.018 H T
/// sqrt(g / (D (s - 1))), s = 2.65.
f64 outer_closure_depth(f64 height_m, f64 period_s, f64 grain_mm) noexcept;
/// The height at which a deep-water wave breaks, carried over contours parallel to the shore by
/// its energy flux with linear theory, the breaker index 0.78 and `cos_angle` the cosine of its
/// deep-water angle to the shore's normal.
f64 breaker_height(f64 height_m, f64 period_s, f64 cos_angle) noexcept;
/// Stockdon et al. 2006's run-up exceeded by 2% of waves, metres above the still level, for a
/// deep-water height and period on a face of slope `slope`.
f64 run_up(f64 height_m, f64 period_s, f64 slope) noexcept;
/// The section's depth below the level at `x_m` metres seaward of the waterline: the face's plane
/// where it is shallower than Dean's curve, the curve beyond (docs/subsystems/landform.md, "The
/// coast"). Negative x is the land: minus the face's height there, capped at `berm_m`.
f64 beach_depth(f64 x_m, f64 scale, f64 slope, f64 berm_m) noexcept;

/// A point's whole flow, in the units its shares among its receivers and `route_divided`'s
/// discharge are kept in: whole numbers, so a divided flow is conserved exactly.
inline constexpr u32 k_share_one = 32768;

/// A receiver code: the neighbour a point drains to, east first and then clockwise seen from above
/// with +z down the page (east, south-east, south, south-west, west, north-west, north,
/// north-east). `k_no_receiver` marks a held point, which drains out of the region.
inline constexpr u8 k_no_receiver = 255;
inline constexpr i32 k_receiver_dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr i32 k_receiver_dz[8] = {0, 1, 1, 1, 0, -1, -1, -1};

/// The filled surface's unit: 1/1024 mm, and the flood's epsilon is one of them.
inline constexpr i64 k_fill_units_per_mm = 1024;

/// A ground to start from: an `engine.terrain.heightfield/1` grid, as read.
struct StartField {
  u32 width = 0;
  u32 height = 0;
  i64 spacing_mm = 0;
  i64 origin_x_mm = 0;
  i64 origin_z_mm = 0;
  f64 vertical_mm_per_unit = 1000.0;
  Vector<f32> data;
};

/// Where the run's material went, cubic metres (landform.md, "Deposition"). `eroded` is what the
/// rivers cut and the slopes shed; all of it was `deposited` again, `left` through a held point,
/// or was shed by the last step and never carried: eroded = deposited + left + unrouted. And the
/// ground's volume changed by what was raised less what went: change = uplifted - left -
/// crept_out - settled - unrouted, to the whole millimetres the heights round to.
struct Budget {
  f64 uplifted_m3 = 0.0;
  f64 eroded_m3 = 0.0;
  f64 deposited_m3 = 0.0;
  /// Carried by the rivers through the held points: an open edge, the sea's edge, an outlet.
  f64 left_m3 = 0.0;
  /// Creep across the held points (the diffusion's exchange with them); negative is a gain.
  f64 crept_out_m3 = 0.0;
  /// What the settling to the critical slope after the last step removed. It has nowhere to go
  /// and leaves the model.
  f64 settled_m3 = 0.0;
  /// What the last step's slopes shed, which no later step's rivers carried: it leaves too.
  f64 unrouted_m3 = 0.0;
  /// The solved ground's volume less the starting ground's, before the heights are rounded.
  f64 change_m3 = 0.0;
  /// The ice's own load (landform.md, "Moraines"): what the glaciers' ice brought to their
  /// snouts over the run (the sketch's `carried` share of the sediment that reached each one),
  /// what of it was laid as ridges, and what was lodged under the ice. Delivered less the
  /// ridges is what the ridges had no room for, which went on with the meltwater. All three
  /// are inside `deposited_m3` and `eroded_m3` as any other sediment is.
  f64 ice_delivered_m3 = 0.0;
  f64 moraine_m3 = 0.0;
  f64 lodged_m3 = 0.0;
  /// What the rivers laid under the sea, all of it below the wave base (landform.md, "The
  /// coast"): inside `deposited_m3`.
  f64 shelf_m3 = 0.0;
  /// What the coast's stand laid as beach where its section stands above the ground, and took
  /// where the ground's sediment stood above it. Outside the rivers' identity; the ground's
  /// change gains placed less taken.
  f64 beach_placed_m3 = 0.0;
  f64 beach_taken_m3 = 0.0;
};

/// Where a solve's wall-clock seconds went, by part (landform.md, "Performance notes"). For a
/// person and the command's summary; nothing reads it back and no decision depends on it.
struct Timing {
  f64 uplift_s = 0.0;   // the uplift and the rock's resistance, every step
  f64 flood_s = 0.0;    // the flood, the lakes and the receivers
  f64 network_s = 0.0;  // the step's network laid out in the sweeps' sequence
  f64 sweeps_s = 0.0;   // the rivers' sweeps
  f64 books_s = 0.0;    // the step's books: lakes, the sea, what each point lost or gained
  f64 slopes_s = 0.0;   // lateral erosion, the ice's deposit, diffusion, the slope limiter
  f64 settle_s = 0.0;   // the settling to the critical slope after the last step
  f64 stand_s = 0.0;    // the coast's stand: the waterline, its distance, exposure, the section
  f64 fields_s = 0.0;   // the outputs and their routing
};

/// A solved region. Every array is `width * height`, rows of x in order of z, on one lattice:
/// point (i, j) stands at `origin + (i, j) * spacing`.
struct Landform {
  u32 width = 0;
  u32 height = 0;
  i64 spacing_mm = 0;
  i64 origin_x_mm = 0;
  i64 origin_z_mm = 0;
  bool has_sea = false;
  i64 sea_level_mm = 0;
  /// The sea's spring tidal range, whole millimetres.
  i64 tide_mm = 0;
  /// The depth the sea's bed was built to and no higher, metres (zero with no sea).
  f64 wave_base_m = 0.0;
  u32 steps = 0;
  /// The sweeps the rivers' solve took over the whole run (landform.md, "Deposition"): one a step
  /// with nothing laid down, a handful with; steps times the limit says a step did not settle.
  u32 river_sweeps = 0;
  /// The levels of the last step's flow network: its longest river, in points (landform.md,
  /// "The rivers' sweeps"). The lattice over this is how wide a level is.
  u32 network_levels = 0;
  i64 step_yr = 0;
  /// What `route` takes to find these receivers again: the sketch's seed, salted.
  u64 route_seed = 0;
  /// The ground, whole millimetres.
  Vector<i32> height_mm;
  /// The cells that drain through the point, itself included (times spacing squared: the area).
  Vector<u32> drainage_area;
  /// The receiver code, or `k_no_receiver` at a held point.
  Vector<u8> receiver;
  /// The fall to the receiver over the distance to it, parts per million; zero or less where the
  /// way out crosses a depression (the receiver is the filled surface's).
  Vector<i32> slope;
  /// What the last step removed, micrometres a year, net of what it laid down: negative where
  /// the ground gained (zero for a run of no steps).
  Vector<i32> erosion_rate;
  /// The sediment under the point, whole millimetres: what the rivers, the lakes, the sea and
  /// creep laid down and nothing has removed since. Zero where the rock is at the surface, and
  /// for a run of no steps. The rock's surface is the height less this.
  Vector<u32> sediment;
  /// The resistance of the rock at the point's surface, 0 (the sketch's erodibility) to 255 (the
  /// most resistant: the erodibility over the contrast, the resistant critical slope): the bed
  /// of the sketch's formation the solved surface cuts, or the sketch's uniform value.
  Vector<u8> resistance;
  /// The cells whose water passes through the point when flow divides where the ground is
  /// gentle (landform.md, "Flow that divides"), rounded to whole cells: `drainage_area` is the
  /// one thread down each point's receiver, this is where the water spreads over a fan or a
  /// plain. The two are equal wherever nothing above the point divided.
  Vector<u32> discharge;
  /// A channel's width at the point, whole millimetres: the sketch's `channel_width_m` times the
  /// square root of the discharge in square kilometres.
  Vector<u32> channel_width;
  /// The ice over the point at the last step it stood, whole millimetres (landform.md, "Ice"):
  /// the glacier's extent, and with a sketch whose ice lasts to the run's end the ice of today.
  /// Empty of ice is zero; a sketch with no ice age writes zeros.
  Vector<u32> ice;
  /// The till under the point, whole millimetres: the part of `sediment` the ice left (lodged
  /// under it, or laid as a moraine at its snout) and nothing has removed since.
  Vector<u32> till;
  /// The thickest ice that ever stood over the point, whole millimetres: the trimline is its
  /// edge. Zero where no ice stood.
  Vector<u32> ice_max;
  /// The part of `till` that was laid as a ridge where a snout stood, whole millimetres
  /// (landform.md, "Moraines"), and `moraine_crest` the ridge's crest line: zero off a crest,
  /// else one more than the stand that laid it (1 is the ice's first line, 2 its first
  /// `stands` entry). A crest whose ridge was since worn away keeps its mark while any of the
  /// ridge's till is left under it.
  Vector<u32> moraine;
  Vector<u8> moraine_crest;
  /// The sediment under the point that settled in standing water (a lake's delta and its
  /// levelled floor), whole millimetres: the part of `sediment` that makes a flat. With `fill`
  /// (the water standing now) it is the flat's extent: a lake, or a lake filled.
  Vector<u32> lakebed;
  /// The rock's surface, whole millimetres: the height less `sediment`. Under a lake's fill,
  /// an outwash plain or a moraine it is the floor the ice and the rivers cut.
  Vector<i32> rock_floor;
  /// The coast (docs/subsystems/landform.md, "The coast"; all zero with no sea, and all but
  /// `shore_distance` zero off the beach's band or with no stand):
  /// - `shore_distance`: millimetres from the still waterline of the solved ground, positive
  ///   over the sea (the points under the level that connect to a sea edge), held to 2 km;
  /// - `substrate`: a `Substrate`; `grain`: the beach's median grain, micrometres;
  /// - `exposure`: the energy-weighted breaker height of the climate that reaches the shore,
  ///   millimetres; `wave_direction`: where those waves come from, 256ths of a turn clockwise
  ///   from north;
  /// - `beach_slope`: the face's tan beta, parts per million; `berm`: the berm's crest above the
  ///   level, millimetres.
  Vector<i32> shore_distance;
  Vector<u8> substrate;
  Vector<u32> grain;
  Vector<u16> exposure;
  Vector<u8> wave_direction;
  Vector<i32> beach_slope;
  Vector<u32> berm;
  /// The shore's segments the stand found and how many of them a wave of the climate reached.
  u32 shore_segments = 0;
  u32 exposed_segments = 0;
  /// How many times lateral erosion took a bank's point over the run.
  u32 banks_taken = 0;
  /// The steps the ice stood for.
  u32 ice_steps = 0;
  Budget budget;
  Timing timing;
  /// How deep the point lies under its depression's spill, whole millimetres; zero on draining
  /// ground.
  Vector<u32> fill;
};

struct SolveOptions {
  /// Null runs every loop on the calling thread. The result is the same bytes either way.
  jobs::JobSystem* jobs = nullptr;
  /// The rivers' sweeps run a level of the flow network on the job system where the level holds
  /// twice this many points or more, in chunks of this many (landform.md, "The rivers' sweeps").
  /// A knob for the tests, which set it small so a small lattice takes the parallel path: the
  /// bytes are the same at any value.
  u32 level_grain = 4096;
};

/// Every range the schema states, with a sentence naming the field that is outside one.
bool validate_sketch(const Sketch& sketch, std::string& error);

/// Solves `sketch`. `start` is the ground to refine when the sketch names one (the caller reads
/// it), or null to start from the base level with the seed's roughness.
bool solve(const Sketch& sketch, const StartField* start, const SolveOptions& options,
           Landform& out, std::string& error);

/// One step's parts, for tests and for a tool that routes a ground it did not solve: the flood,
/// the receivers and the drainage area of `height_mm` with `held` points (one byte each, non-zero
/// held) as its outlets. Fills `filled` (units of 1/1024 mm), `order` (the flood's pop order: a
/// receiver before its donors), `receiver` and `area`. False when no point is held.
bool route(u32 width, u32 height, const Vector<i32>& height_mm, const Vector<u8>& held, u64 seed,
           jobs::JobSystem* jobs, Vector<i64>& filled, Vector<u32>& order, Vector<u8>& receiver,
           Vector<u32>& area);

/// The divided routing of a ground (landform.md, "Flow that divides"): the flood and the
/// receivers as `route` finds them, then each point's shares of its flow among its lower
/// neighbours (eight a point, of `k_share_one`; all of it to the receiver where the steepest fall
/// is `divide_below_slope` or more) and its discharge in 1 / `k_share_one` of a cell. The held
/// points' discharges sum to the lattice exactly. False when no point is held.
bool route_divided(u32 width, u32 height, i64 spacing_mm, const Vector<i32>& height_mm,
                   const Vector<u8>& held, u64 seed, f64 divide_below_slope, jobs::JobSystem* jobs,
                   Vector<u8>& receiver, Vector<u16>& share, Vector<u64>& discharge);

// ---- files (src/files.cpp) ----------------------------------------------------------------------

/// Reads a sketch and checks it; `start_path` is the starting heightfield's header resolved beside
/// the sketch, empty when it names none.
bool read_sketch(const std::string& json_path, Sketch& out, std::string& start_path,
                 std::string& error);
/// Reads an `engine.terrain.heightfield/1` header and its grid (the hash checked).
bool read_start_field(const std::string& json_path, StartField& out, std::string& error);

/// What `write_landform` wrote, paths as given.
struct WrittenFiles {
  std::string heightfield;  // <dir>/<name>.json, its grid <name>.f32 beside it
  std::string fields;       // <dir>/<name>.fields.json, each field's grid beside it
};

/// Writes the heightfield (`vertical_mm_per_unit` 1: each float a whole millimetre, exact below
/// 16,777 m) and the field file into `dir`.
bool write_landform(const Landform& land, const Sketch& sketch, const std::string& dir,
                    std::string_view name, WrittenFiles& out, std::string& error);

/// The field file's header, checked against this build's format and version.
bool read_fields(const std::string& json_path, Fields& out, std::string& error);
/// A window of one field: points [x0, x0 + w) by [z0, z0 + h) of the lattice, rows of x in order
/// of z, into `out` (w * h values of the field's type; `out_bytes` must be exactly that). A window
/// that leaves the lattice is refused: the fields hold nothing past the region's edge.
bool read_field_window(const std::string& json_path, const Fields& header, std::string_view field,
                       u32 x0, u32 z0, u32 w, u32 h, void* out, usize out_bytes,
                       std::string& error);

/// A shaded relief for a person to look at: RGBA8, `width * height * 4`, lit from the north-west
/// at 45 degrees, the sea and standing water of half a metre or more tinted, and with
/// `drainage` the channels drawn by their area.
void shaded_relief(const Landform& land, bool drainage, Vector<u8>& rgba);

/// What the ice left, over the same relief: pale blue where ice ever stood (fuller where it
/// was thicker), ochre where a moraine lies with its crest line dark, green where a lake's
/// fill makes a flat, and the lakes that stand.
void ice_map(const Landform& land, Vector<u8>& rgba);

}  // namespace engine::landform
