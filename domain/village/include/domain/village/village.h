#pragma once

// The village capability (ADR-0027; docs/subsystems/village.md, roadmap G69, G206; plan 14 §14.3,
// "The village filler"; docs/experiments/village-growth-grounding.md and village-plan-grounding.md
// for every number): a village that grew along its ways on the scene's ground.
//
// **What it is.** One plan (`plan_village`), a function of the entry, its seed and the ground
// before any edit: each way found over the ground between the places it passes (a least-cost path
// for its slope, wandering by `irregularity`), stationed, its tread's top read off the ground and
// its street's edges wandering and opening into a green where the ways meet; plots taken up along
// the ways nearest the green first, tenements of uneven frontage and depth turned to the slope and
// the sun, cottages squeezed into their gaps and onto the green later, a plot kept for the church;
// on each plot a house asked of the **house filler** (`HouseFn`: from the plot and a seed, a
// footprint, a door and the recipe of its mesh) and stood facing its way, set back from the plot's
// front, on a pad level at the mean of the ground under it — or the plot left empty and counted
// where the ground under the footprint differs by more than a pad should take up, where water or
// a hole is, where the ground is not there, or where the house does not fit; back lanes behind
// the built rows. At `irregularity` 0 the same rules lay out a regular row plan.
//
// **What it hands over.** To the ground, before the ground is made
// (`PlacementGeneratorDesc::plan_ground`): each lane's tread as a `Bench` line, each pad as a
// `Bench` patch, each door's path as a `Bench` line (`ground_edits`). To the entries after it
// (`Placements::shapes`): the treads' and the paths' bands and the pads' polygons (`footprints`),
// so the cover keeps off them. To the host: one placement a house, its mesh a recipe the mesh
// generator registry builds (`PlacementMesh::recipe`), handed by the tile its origin stands on.
//
// **What it is not.** It makes no mesh and knows no building: the description, the mesher and the
// house filler are the structures capability's, reached through `HouseFn`, and a house is to the
// plan a box with a door. It writes no document records (plan 14's `structure.fill_village`: D37's
// records did not exist when this was built; village.md, "Not yet"); the plan is the pure function
// such a writer would call.
//
// **Determinism.** Every place is whole millimetres; a way's search is integer costs with ties to
// the lower cell; a direction is products, sums, one division and one correctly rounded square
// root; a plot's choices are the engine's hash of the seed and the plot's place. No C library call
// decides anything, so the plan is the same bytes on every toolchain (`plan_hash`, pinned by the
// tests).
//
// Checklist (ADR-0027 decision 2), and where each item is:
//   [x] schema types      schemas/village.schema, engine_schema_library
//   [ ] scheduler entry   not needed: nothing ticks; the plan is decided when a scene is read
//   [ ] render passes     not needed: houses are placements, the lanes and pads the ground's
//   [ ] derived data      none of its own: the plan is made at each open (microseconds a plot);
//                         a house's mesh is the structures capability's, in the cache by recipe
//   [ ] protocol methods  not built (village.md, "Not yet")
//   [ ] tunables          not needed: every number is the entry's
//   [x] LOD policy        a house is one placement whose mesh carries its own cluster DAG; a lane
//                         and a pad are ground edits each level of the tile set draws while its
//                         spacing is finer than their width (W98's cell rule)
//   [x] determinism       above
//   [x] zero cost unused  a scene with no `village` entry never calls it (the registry)
//   [x] docs, tests, size table

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/footprint_list.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/scene.h>
#include <schemas/village.h>
#include <string>

namespace engine::village {

// Bumped when a change makes the same entry plan another village. 2: the grown plan (G206): ways
// found over the ground, a green where they meet, plots taken in an order, cottages, back lanes.
inline constexpr u32 k_village_version = 2;

// ---- the house seam -----------------------------------------------------------------------------

// What the plan asks a house for: the plot it will stand on and what its choices are drawn from.
struct HouseAsk {
  i64 frontage_mm = 0;  // the plot's width on the street
  i64 depth_mm = 0;     // back from the street
  i64 setback_mm = 0;   // the house's front from the plot's front
  u64 seed = 0;
  u32 storeys = 1;
  u32 index = 0;  // the plot's place in the plan, for a name
};

// A point of a house's own frame, whole millimetres, y over the pad.
struct RoutePoint {
  i64 x_mm = 0, y_mm = 0, z_mm = 0;
};

// A house as the plan sees it: a box on the ground with a door, in the house's own frame (x and z
// whole millimetres, y up, its origin on the pad), and what its mesh is made from.
struct House {
  // The footprint's box: the outer faces of its walls.
  i64 min_x_mm = 0, min_z_mm = 0, max_x_mm = 0, max_z_mm = 0;
  // The entrance: the middle of its threshold on the wall's outer face, and which way is out (one
  // of the four axis directions).
  i64 door_x_mm = 0, door_z_mm = 0;
  i32 out_x = 0, out_z = -1;
  i64 door_width_mm = 900;
  // How far the frame's origin stands over the pad (a description's origin is its ground floor's
  // top: structures.md): the placement stands the frame this far above the pad's level.
  i64 floor_mm = 0;
  // The mesh: the bytes of a mesh generator's file and the path it would have
  // (`scene_gen::PlacementMesh`), and how far it reaches on the ground from its origin, metres.
  std::string recipe;
  std::string path;
  std::string name;
  f32 reach = 0.0f;
  // **The way through it** (village.md, "The way through a house"): feet on the floor, in the
  // house's frame, from just inside the door through its rooms' doorways, up its stair and to a
  // window, in walking order; and the point outside that window a person there looks at. Empty
  // for a house whose filler says none (a box). What the collision's walk and a flight follow.
  Vector<RoutePoint> route;
  RoutePoint look{};
};

// The house filler: false with a sentence when no house suits the plot.
using HouseFn = bool (*)(void* context, const HouseAsk& ask, House& out, std::string& error);

// ---- the plan -----------------------------------------------------------------------------------

// A station of a way: where it is, the tread's top there, and how far the street's edge stands
// from the line on its left and on its right (the street's own wander and the green), whole
// millimetres. 40 bytes (tests/size_table.cpp).
struct LaneStation {
  i64 x_mm = 0;
  i64 z_mm = 0;
  i64 top_mm = 0;
  i64 left_mm = 0;
  i64 right_mm = 0;
};
// A way as found over the ground (village.md, "The ways"): its stations, its length, its tread's
// half width, and whether it is a back lane behind a row's plots (`back`, of the way `from_lane`).
// The entry's ways come first, in its order; the back lanes after them.
struct LaneLine {
  Vector<LaneStation> stations;
  i64 length_mm = 0;
  i64 half_tread_mm = 0;
  bool back = false;
  u32 from_lane = 0;
};

// Why a plot stands empty.
enum class PlotState : u8 {
  built = 0,
  spare = 1,     // past the entry's house count
  steep = 2,     // the ground under the footprint differs by more than `max_pad_step_m`
  wet = 3,       // a corner or the centre in water, a channel's corridor or a hole
  no_fit = 4,    // the house is wider than the frontage, or deeper than the plot
  refused = 5,   // the house filler had none
  reserved = 6,  // kept for a building nobody builds yet: the church
  outside = 7,   // a corner or the middle off the ground there is (the heightfield's edge)
};

// Who took a plot up (village-growth-grounding.md, "Plots"): a founding tenement along a way, a
// cottage squeezed into a gap between two or onto the green's edge later, or the church's.
enum class PlotKind : u8 {
  tenement = 0,
  cottage = 1,
  church = 2,
};

// A plot and, where one stands, its house. 280 bytes (tests/size_table.cpp).
struct Plot {
  u32 lane = 0;
  u32 order = 0;  // the place in which it was taken up, 0 first
  u8 side = 0;    // 0 left, 1 right
  PlotState state = PlotState::spare;
  u8 quarter_turns =
      0;  // of the house's frame, before the plot's turn, so its door faces the street
  PlotKind kind = PlotKind::tenement;
  u32 house = 0;    // into `VillagePlan::houses`, for a built plot
  u32 storeys = 0;  // asked of its house: the entry's for a tenement, one for a cottage
  // The middle of the plot's front, and the two unit directions of its frame: `along` its front
  // (so that `inward`, the left normal of `along`, points into the plot). A plot's front is turned
  // off square to its way by the slope, the sun and its seed, by up to 20 degrees.
  i64 front_x_mm = 0, front_z_mm = 0;
  f64 along_x = 1.0, along_z = 0.0;
  f64 inward_x = 0.0, inward_z = 1.0;
  i64 frontage_mm = 0, depth_mm = 0;
  i64 along_mm = 0;  // of the plot's middle along its way, from the way's first point
  u64 seed = 0;
  // Where the house's frame stands, the pad's level, and how far the frame's origin (the ground
  // floor's top, structures.md) stands over the pad.
  i64 house_x_mm = 0, house_z_mm = 0, pad_mm = 0;
  i64 floor_mm = 0;
  // The door's threshold in the world; the gate where its path crosses the plot's front, and the
  // ground there; where the path meets the tread's edge, and the way's line beside it.
  i64 door_x_mm = 0, door_z_mm = 0;
  i64 gate_x_mm = 0, gate_z_mm = 0, gate_top_mm = 0;
  i64 path_x_mm = 0, path_z_mm = 0, path_top_mm = 0;
  i64 lane_x_mm = 0, lane_z_mm = 0;
  // The pad's corners in the world, in order round it.
  i64 pad_x_mm[4] = {0, 0, 0, 0};
  i64 pad_z_mm[4] = {0, 0, 0, 0};
};

struct VillagePlan {
  Vector<LaneLine> lanes;
  Vector<Plot> plots;    // in the order they were taken up
  Vector<House> houses;  // the built plots', in the plots' order
  u32 built = 0, steep = 0, wet = 0, no_fit = 0, refused = 0, spare = 0, outside = 0;
  u32 cottages = 0;     // of the built
  u32 back_lanes = 0;   // of `lanes`
  bool church = false;  // a plot is reserved for it
  // Where the ways meet (or the first way's middle): the green's middle and the well, whole
  // millimetres, the ground's top there.
  i64 focus_x_mm = 0, focus_z_mm = 0, focus_top_mm = 0;
  bool well = false;
  // "hamlet" under six houses, "village" from six (the grounding's threshold).
  const char* size_class = "hamlet";
  // Why the first plot left empty by its house was: the filler's sentence for a refusal, the sizes
  // for a house that did not fit. Empty when every plot asked got its house.
  std::string first_refusal;
};

// The entry's numbers in range. False with a sentence.
bool layer_in_range(const VillageLayer& layer, std::string& error);

// **The planner.** `ground` is the ground before any edit (`scene_gen::Context::uncut()`);
// `context` is asked what is excluded (water, channels, holes) and nothing else. False with a
// sentence for an entry out of range or a ground that is not finite.
bool plan_village(const VillageLayer& layer, const scene_gen::Ground& ground,
                  const scene_gen::Context& context, HouseFn house, void* house_context,
                  VillagePlan& out, std::string& error);

// The same plan with the house filler the generator "village" plans with (the structures
// capability's rural filler), on `context.uncut()`: what a tool or a test calls to get the scene's
// own village.
bool plan_scene_village(const VillageLayer& layer, const scene_gen::Context& context,
                        VillagePlan& out, std::string& error);

// A hash of everything the plan decided (the stations, the plots, each house's box, door and
// recipe): what a test pins and a second toolchain reproduces.
u64 plan_hash(const VillagePlan& plan);

// **The ground edits** (`engine.scene.TerrainEdit`, mode `Bench`), appended in the plan's order:
// a line a lane (its stations, the tread's width), a patch a pad (its four corners, flat at the
// pad's level) and a line a path (the door to the tread's edge).
void ground_edits(const VillageLayer& layer, const VillagePlan& plan,
                  Vector<scene::TerrainEdit>& out);

// **The shapes the village occupies** (whole millimetres): a band a stretch of tread and a path,
// a polygon a pad. `tile_mm` 0 hands every shape; otherwise those whose disc reaches within
// `reach_mm` of tile (`tile_x`, `tile_z`).
void footprints(const VillageLayer& layer, const VillagePlan& plan, scene_gen::FootprintList& out,
                i64 tile_mm = 0, i64 tile_x = 0, i64 tile_z = 0, i64 reach_mm = 0);

// The palette layers the village's wear names (village.md, "The wear"): an empty name leaves its
// regions out.
struct GroundLayers {
  std::string_view verge = "verge";  // the street between the plots' fronts, outside the tread
  std::string_view worn = "worn";    // the tread's ragged margin, the paths and the yards
  std::string_view tread = "lane";   // the tread's middle, where the wheels and the feet go
};

// **The wear as mask regions** (`engine.scene.TerrainMaskRegion`), in the order they are laid, each
// over the last: a lane, the street's whole width as verge, the tread and `k_wear_margin_mm` past
// each edge as worn ground, and the tread's middle; then a built plot, its **door yard** and path
// as one outline, the bare ground `yard_m` each side of the door from the wall to the plot's front
// and the path on from there under the tread's margin. Each is feathered inward, so a band's edge
// is where the two materials' heights decide (the masks' height blend), not a line. What an author
// pastes into `terrain.masks.regions`, since a planned mask region is not handed across
// (scene_gen.md, "What an entry plans for the ground").
inline constexpr i64 k_wear_margin_mm = 450;
JsonValue ground_regions_json(const VillageLayer& layer, const VillagePlan& plan,
                              const GroundLayers& names = {});

// **A plot's boundary** (village.md, "The boundaries"; the grounding's "Boundaries"): the line a
// hedge, a fence, a bank or a wall stands on, whole millimetres, x and z pairs. A built plot is
// bounded along its front, its two sides and its back; a side two built neighbours share is one
// line's, the earlier plot's, so nothing stands twice. The front carries one **gateway** where the
// door's path crosses it: its middle as arc length from the line's first point.
struct BoundaryLine {
  u32 plot = 0;  // into `VillagePlan::plots`
  bool closed = false;
  Vector<i64> points_mm;
  Vector<i64> gateways_mm;
};
void plot_boundaries(const VillageLayer& layer, const VillagePlan& plan, Vector<BoundaryLine>& out);

// The plan for a person or a tool: the lanes' lengths, each plot's place, state and house, the
// counts and the size class.
JsonValue plan_json(const VillageLayer& layer, const VillagePlan& plan);

// Where a built plot's house stands: the placement a host draws (the mesh index is the caller's).
scene_gen::Placement house_placement(const Plot& plot);
// A point of the house's own frame (whole millimetres, y over the pad) in the world.
WorldPos house_point(const Plot& plot, i64 x_mm, i64 y_mm, i64 z_mm);

// A stand-in house filler for tests and for a build with no structures capability behind it: a box
// `length_mm` by `width_mm` with its door in the middle of its long side, no recipe.
struct BoxHouse {
  i64 length_mm = 9000;
  i64 width_mm = 4600;
};
bool box_house(void* context, const HouseAsk& ask, House& out, std::string& error);

}  // namespace engine::village
