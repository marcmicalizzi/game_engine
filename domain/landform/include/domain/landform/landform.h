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
inline constexpr u32 k_solver_version = 4;

inline constexpr std::string_view k_fields_format = "engine.landform.fields";
inline constexpr u32 k_fields_version = 2;

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
  u32 steps = 0;
  /// The sweeps the rivers' solve took over the whole run (landform.md, "Deposition"): one a step
  /// with nothing laid down, a handful with; steps times the limit says a step did not settle.
  u32 river_sweeps = 0;
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
  Budget budget;
  /// How deep the point lies under its depression's spill, whole millimetres; zero on draining
  /// ground.
  Vector<u32> fill;
};

struct SolveOptions {
  /// Null runs every loop on the calling thread. The result is the same bytes either way.
  jobs::JobSystem* jobs = nullptr;
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
/// at 45 degrees, the sea tinted, and with `drainage` the channels drawn by their area.
void shaded_relief(const Landform& land, bool drainage, Vector<u8>& rgba);

}  // namespace engine::landform
