#pragma once

// The routes capability (ADR-0027; docs/subsystems/routes.md, roadmap W96; the proof of concept's
// solver needs 3.6): footpaths over a region's ground, as geometry and as wear.
//
// **What it is.** One plan (`ensure_plan`, a derived file keyed by the layer and the ground) of a
// network of footpaths: each route's waypoints joined, pair by pair, by the least-cost path on a
// lattice of the ground, where a step costs the time Tobler's hiking function gives for its grade
// (walked both ways, so the mean of the two directions'), a turn costs seconds by its angle (the
// search carries a heading, so a climb is a few long legs joined by switchbacks rather than a
// zigzag of equal cost), and no step climbs or falls more than the grade cap where a path under it
// exists; every route after the first walks the earlier ones' steps for `trodden` of their time,
// so branches merge. The union of the steps is cut at its junctions into lines, each line's corners
// cut (Chaikin's passes, never taking a stretch over the cap), and stationed every `station_m` in
// whole centimetres. A station's **tread top** is the lowest ground across the tread there (a full
// bench: the tread stands on cut ground, the Forest Service's preferred construction), held under
// the cap along the line by the lower envelope of cones of slope `grade_cap` over those heights,
// and sunk `sink_m`.
//
// **What it writes** (`engine-content routes`): each line as an `engine.scene.TerrainEdit` of mode
// `Bench` (`tread_edits_json`), which the tile set writes into its heights (W98: cut on the uphill
// side, filled on the downhill, the tread flat across its width); the tread as a terrain mask
// region for its material (`tread_regions_json`); and the tread as band footprints
// (`tread_footprints`) that `scene_gen::Exclusion::add_footprints` holds, so a placement asking
// `excluded_mm` leaves the tread bare.
//
// **Water** (routes.md, "Water"): handed the placements' exclusion, the search never steps into a
// water body and pays `ford_penalty` for a step touching a channel's corridor, so a stream is
// forded short and square; `crossings_json` lists the fords where a footbridge would stand.
//
// **Wear** (`RouteWear`, `wear_factor`): the share of the cover trampling leaves at a distance
// from the tread's edge — `tread_cover` on the tread, rising to 1 at `wear_m` past its edge by a
// smoothstep — which any placement can ask for a density or a height.
//
// **What the placement generator does.** "routes" opens the plan (named on a ground that carries
// its treads) and places nothing yet: a route's meshes (exposed roots and stones, steps) are later
// rows'. It is what makes a scene's routes entry a scene's, so a ground that drifts from the plan
// a scene names is refused at load.
//
// **Determinism.** The lattice's heights are whole millimetres; a step's grade test is products of
// integers and one correctly rounded square root; a step's time is `det::exp` (never the C
// library's), rounded once to whole milliseconds; the search's queue is ordered by (cost, state),
// a total order, and the jobs sample the lattice a row each. So the plan is the same bytes on every
// toolchain, at every thread count, from every host.
//
// Checklist (ADR-0027 decision 2), and where each item is:
//   [x] schema types      schemas/routes.schema, engine_schema_library
//   [ ] scheduler entry   not needed: nothing ticks; the plan is decided when a scene is read
//   [ ] render passes     not needed: the tread is the ground's, drawn by the terrain
//   [x] derived data      the plan file, `<ddc>/routes/plan-<key>.route-plan`
//   [ ] protocol methods  not needed
//   [ ] tunables          not needed: every number is the layer's
//   [x] LOD policy        the tread is a ground edit: each level of the tile set draws it while its
//                         spacing is finer than the tread's width (W98's cell rule) and closes it
//                         past that, and the mask region carries it on as colour
//   [x] determinism       above
//   [x] zero cost unused  a scene with no `routes` entry never calls it (the registry)
//   [x] docs, tests, size table

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/world.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/routes.h>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::routes {

// Bumped when a change makes an old plan wrong: it is in every plan's key.
inline constexpr u32 k_routes_version = 1;

// What the stretch from a station to the next is: **steep**, a lattice step over the cap taken
// where no path under it existed; a **ford**, a lattice step with an end in a channel's corridor
// (`Exclusion`'s channel kind), where the route wades (routes.md, "Water").
inline constexpr u32 k_station_steep = 1u;
inline constexpr u32 k_station_ford = 2u;

// A station of a line: where it is (x and z whole centimetres, y the tread's top, whole
// millimetres, metres), the ground under it there, and what its stretch to the next is.
struct Station {
  WorldPos at{};
  f32 ground = 0;  // the ground at the station's centre, metres (to the millimetre)
  u32 flags = 0;   // k_station_steep, k_station_ford
};

// A line of the network: the stretch between two of its ends or junctions.
struct RouteLine {
  Vector<u32> cells;  // lattice indices, in order
  Vector<Station> stations;
  f64 length_m = 0;     // along the stations
  u32 steep_steps = 0;  // lattice steps over the cap
  f64 over_cap_m = 0;   // metres of stations over the cap (steep stretches)
  u32 switchbacks = 0;  // turns past 90 degrees on the smoothed line
  f64 max_hold_m = 0;   // the deepest the cap held the tread under the lowest ground
};

// The network over a layer's region.
struct RouteNetwork {
  u32 width = 0;  // lattice points along x
  u32 depth = 0;  // along z
  i64 x_mm = 0;   // point (0, 0), world millimetres
  i64 z_mm = 0;
  i64 spacing_mm = 0;
  Vector<i64> ground_mm;  // the lattice's heights, whole millimetres (empty when read from a file)
  // What water covers each lattice point: 0 dry, 1 a channel's corridor, 2 a water body (empty
  // when read from a file, or when the planner was handed no exclusion).
  Vector<u8> wet;
  Vector<RouteLine> lines;
  u32 legs = 0;         // waypoint pairs searched
  u32 steep_legs = 0;   // legs with no path under the cap
  u32 unreached = 0;    // legs with no path at all: a waypoint cut off by a water body
  f64 length_m = 0;     // every line, summed
  f64 over_cap_m = 0;   // every line's
  u32 switchbacks = 0;  // every line's
  u32 fords = 0;        // runs of ford stretches, every line's
  f64 ford_m = 0;       // their metres
};

// The layer's numbers in range and its region aligned to its spacing. False with a sentence.
bool layer_in_range(const RouteLayer& layer, std::string& error);

// **The planner**: the network over the layer's region standing on `ground`. `jobs` (nullable)
// samples the lattice a row a job; the result does not depend on it. False with a sentence for a
// layer out of range, an absent or non-finite ground, or a region past `max_samples`.
// `water` (nullable; routes.md, "Water"): the scene's channels and water bodies as the placements'
// exclusion holds them — a step into a body is never taken, and a step with an end in a channel's
// corridor costs `ford_penalty` times its time, so a route crosses a stream where it is narrow and
// square to it; null, no water.
bool plan_routes(const RouteLayer& layer, const scene_gen::Ground& ground, jobs::JobSystem* jobs,
                 RouteNetwork& out, std::string& error,
                 const scene_gen::Exclusion* water = nullptr);

// **One plan** (routes.md, "One plan"), as the channels' is: made once and kept as
// `<derived-data root>/routes/plan-<key>.route-plan`. The key is the layer (every field but
// `plan`), `k_routes_version` and the ground's heights at every lattice point. A layer that names
// its plan (the key `engine-content routes` printed) reads it and plans nothing, whatever the
// ground: the treads it wrote changed the ground the key was made from.
struct RoutePlan {
  u64 key = 0;
  u64 layer_hash = 0;
  RouteNetwork network;
  bool planned = false;  // made by this call (a miss); false: read from its file
};
u64 plan_layer_hash(const RouteLayer& layer);
std::string plan_key_text(u64 key);
std::string plan_path(std::string_view dir, u64 key);
// `<ddc>/routes`, `ddc` the host's root when it named one, else the repository's found from
// `scene_dir`, else `<scene_dir>/ddc`; made when it is not there.
bool derived_dir(std::string_view ddc, std::string_view scene_dir, std::string& out,
                 std::string& error);
bool ensure_plan(const RouteLayer& layer, const scene_gen::Context& context, std::string_view dir,
                 RoutePlan& out, std::string& error);
// The file's bytes (integers and IEEE bits, little-endian, closed by a hash of the whole), and a
// plan from them; `read_plan` refuses a short file, another version, or arrays that disagree.
std::string plan_bytes(const RoutePlan& plan);
bool read_plan(std::string_view bytes, RoutePlan& out, std::string& error);
// How many times this process has run the planner: what a test holds a cached load to.
u64 plans_made() noexcept;

// **The treads as ground edits** (`engine.scene.TerrainEdit`, the `tiles` block's `edits`): one
// `Bench` line a network line, `{name, mode, points, width_m, bank_slope, fill_slope}`, its points
// the stations thinned to those a straight run between two of them misses by more than 5 mm in x,
// z or y, y the tread's top. A JSON array in the network's order.
JsonValue tread_edits_json(const RouteLayer& layer, const RouteNetwork& network);
// **The tread's material** (`engine.scene.TerrainMaskRegion`): one polygon a line, the tread's two
// edges, naming `tread_material`; empty when the layer names none.
JsonValue tread_regions_json(const RouteLayer& layer, const RouteNetwork& network);
// **The tread as footprints** (W98's bands, whole millimetres): one band a straight run of
// stations (within 5 mm), its half-width the tread's. Appended to `out`.
void tread_footprints(const RouteLayer& layer, const RouteNetwork& network,
                      Vector<scene_gen::Footprint>& out);
// **The crossings** (routes.md, "Water"): each run of ford stretches as `{line, from, to, at,
// length_m}` — the line's index, its first and last station, the run's middle station (x, y the
// tread's top, z) and its length — where a bridge or stepping stones would stand (row G147's). A
// JSON array in the network's order.
JsonValue crossings_json(const RouteNetwork& network);

// **Wear** (routes.md, "Wear"): the share of the cover left `d` metres past the tread's edge
// (negative inside it): `tread_cover` on the tread, 1 at `wear_m` and beyond, a smoothstep between.
// Never decreasing in `d`.
f64 wear_factor(f64 d, f64 wear_m, f64 tread_cover) noexcept;

// The wear of a network, asked at a point: the distance to the nearest tread's edge and the cover
// it leaves. Built once from a plan; read-only after, so any number of threads may ask.
class RouteWear {
 public:
  void build(const RouteLayer& layer, const RouteNetwork& network);
  bool empty() const noexcept { return segments_.empty(); }
  u32 segment_count() const noexcept { return segments_.size(); }
  // Metres from the world point (whole millimetres) to the nearest tread's edge, negative inside a
  // tread, or a very large number (over 1e20) when no edge is within `reach_m`.
  f64 edge_distance_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept;
  // `wear_factor` of that distance: 1 where no tread is within `wear_m`.
  f64 cover_mm(i64 x_mm, i64 z_mm) const noexcept;

 private:
  struct Segment {
    f64 ax = 0, az = 0, bx = 0, bz = 0;  // metres
    f64 half = 0;
  };
  struct Cell {
    i64 key_z = 0;
    i64 key_x = 0;
    u32 segment = 0;
  };
  Vector<Segment> segments_;
  Vector<Cell> cells_;  // sorted by (z, x, segment)
  f64 wear_m_ = 2, tread_cover_ = 0;
};

}  // namespace engine::routes
