#pragma once

// Footbridges where a route fords a stream (roadmap G147; docs/subsystems/bridges.md, the sources
// and numbers in docs/experiments/footbridges.md).
//
// **What it is.** Two generators in one capability. The placement generator `bridges` reads a layer
// of crossings (`engine.bridges.Layer`, written by `engine-content bridges` from a route's plan,
// the scene's channels and its ground) and a tile realises the bridge whose centre falls in it, as
// one mesh the tile hands over as a recipe (scene_gen.md, "A tile's meshes"). The mesh generator
// `bridge` solves one bridge from its recipe (`engine.bridges.Bridge`, `<name>.bridge.json`):
// `plan_bridge` chooses the form a footpath would build there — stepping stones, a clapper bridge,
// a foot log, or log stringers under a plank deck — by the crossing's bankfull width and depth and
// the bed's stone, and solves every member in whole millimetres; `solve_bridge` makes each member a
// closed solid, stone members and timber members in one mesh of two materials.
//
// **What it writes to its neighbours** (bridges.md, "The ground and the neighbours"): each bridge's
// approaches as `Fill` ground edits from the deck's ends to the tread (`approach_edits_json`),
// handed to the ground through the host's hook before the ground is made
// (`PlacementGeneratorDesc::plan_ground`, G186), and its plan as a polygon footprint
// (`bridge_footprint`) handed with every tile it reaches and with the whole read
// (`Placements::shapes`), so the bed's stones, the sward and scatter keep out from under it. A
// scene names its `bridges` entry and pastes neither; `engine-content bridges` still prints both
// for a person to read.
//
// **What it is not.** No road or cart bridge, no arch, no suspension span; no response of the water
// to a pier (systems/water is not this capability's); no collision of its own (a tile's mesh is
// walked on through the collision's proxy of it, R145).
//
// Checklist (ADR-0027 decision 2), and where each item is:
//   [x] schema types      schemas/bridges.schema, engine_schema_library
//   [ ] scheduler entry   not needed: nothing ticks; a bridge is decided when a scene is read
//   [ ] render passes     not needed: a bridge is a mesh the renderer draws as any other
//   [x] derived data      the recipes, built through the mesh generator registry and the cache
//   [ ] protocol methods  not needed
//   [ ] tunables          not needed: every number is the layer's or a cited constant
//   [x] LOD policy        the mesh's cluster DAG: its finest level the members, its root the far
//                         form (bridges.md, "Forms and the LOD policy")
//   [x] determinism       integer millimetres, one seed per crossing, no C library in a decision
//   [x] zero cost unused  a scene with no `bridges` entry never calls it (the registry)
//   [x] docs, tests, size table, bench

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/bridges.h>
#include <string>
#include <string_view>

namespace engine::bridges {

// The solver's version, in every recipe's source hash: a change that moves a member bumps it.
inline constexpr u32 k_bridge_version = 2;

// ---- the grounded constants (docs/experiments/footbridges.md, each against its source) ---------

// Stepping stones: a brook whose bankfull depth is at most this and whose width is at most
// `k_stones_max_width_mm` (the Forest Service's notebook fords hikers where the water is 400 to
// 600 mm deep most of the season; a line of stones is a ford's dry footing).
inline constexpr i32 k_stones_max_depth_mm = 500;
inline constexpr i32 k_stones_max_width_mm = 4000;
// Centre to centre along the path: under the shorter adult step on level ground (0.66 m, Murray's
// gait values), shortened for a step from stone to stone. A choice from a step length, said so.
inline constexpr i32 k_stone_pitch_mm = 600;
// A stone's top above the water at the scene's stage, and how deep its foot is set in the bed.
inline constexpr i32 k_stone_rise_mm = 150;
inline constexpr i32 k_stone_bed_mm = 150;
// Clapper slabs: no slab longer than this centre to centre of its piers (Tarr Steps: slabs 2 to
// 2.6 m over gaps of 1.2 to 1.8 m, the largest 2.4 m by 1.5 m), a deck this wide (Tarr Steps 1.5 m;
// the TCV's widest footbridge deck 1.2 m), piers this wide along the path, founded below the bed.
inline constexpr i32 k_clapper_max_slab_mm = 2400;
inline constexpr i32 k_clapper_deck_mm = 1200;
inline constexpr i32 k_clapper_pier_mm = 700;
inline constexpr i32 k_pier_found_mm = 200;
// A clapper is only built where the stone is to hand (the TCV): a bed whose D84 is at least this
// (the cobble-boulder break, 256 mm), up to the width the slabs serve on four piers.
inline constexpr i32 k_clapper_min_d84_mm = 256;
inline constexpr i32 k_clapper_max_width_mm = 8000;
// The foot log: at least 457 mm (18 in) through, its top hewn flat at least 250 mm (10 in) wide
// (the notebook), over up to this span; log stringers past it (the TCV: 250 mm logs at about 5 m
// to 400 mm at about 10 m; designs past 9 m want an engineer).
inline constexpr i32 k_foot_log_mm = 480;
inline constexpr i32 k_foot_log_max_span_mm = 5000;
inline constexpr i32 k_stringers_max_span_mm = 9000;
// The stringer deck: the Forest Service's least clear width for hikers (3 ft, 914 mm), the TCV's
// 900 mm; planks 150 by 50 mm with 20 mm gaps (the TCV's boards; the notebook's 3/4 in gaps).
inline constexpr i32 k_deck_mm = 900;
inline constexpr i32 k_plank_mm = 150;
inline constexpr i32 k_plank_thick_mm = 50;
inline constexpr i32 k_plank_gap_mm = 20;
// A handrail 1070 mm (42 in) over the deck (the Forest Service, the NPS), fitted where the deck
// stands more than `k_rail_fall_mm` over the bed. No handbook gives the fall height: a choice.
inline constexpr i32 k_rail_mm = 1070;
inline constexpr i32 k_rail_fall_mm = 1000;
// Where a sill or an abutment stands: its inner face this far back from the bank's edge (the
// bankfull width's), so the span is at least the bankfull width and no support is in the channel
// (Forest Service, "Locating your trail bridge").
inline constexpr i32 k_setback_mm = 400;
// Freeboard: the timber's underside over the bankfull level by the bankfull depth (flood stage
// about twice the bankfull depth over the bed, the same page), at least this. A clapper keeps none:
// its slabs lie at the banks' level and floods go over (Tarr Steps, rebuilt after 1942, 1952,
// 1979).
inline constexpr i32 k_min_freeboard_mm = 300;

// ---- the plan
// ------------------------------------------------------------------------------------

// What a member is, and what it is made of.
enum class MemberKind : u8 { stone, slab, pier, abutment, log, sill, stringer, plank, post, rail };
inline constexpr u32 k_member_kinds = 10;
// The two materials of a bridge's mesh: 0 the stone, 1 the timber.
inline constexpr u8 k_stone = 0;
inline constexpr u8 k_timber = 1;
// The shape a member is drawn as: a block with a rough outline (a stone), a sawn box, or a log
// along the bridge's x or z.
enum class Shape : u8 { rough, box, log_x, log_z };

// One member, in the bridge's frame: x along the path, z across it, y up from the crossing's bed,
// whole millimetres. `support` is the member it rests on (its index, lower than its own), or -1
// for the ground; `hewn` the height a log's top is hewn flat at (0: round).
struct Member {
  i32 x0 = 0, x1 = 0;
  i32 y0 = 0, y1 = 0;
  i32 z0 = 0, z1 = 0;
  i32 support = -1;
  i32 hewn = 0;
  u32 seed = 0;
  MemberKind kind = MemberKind::stone;
  Shape shape = Shape::rough;
  u8 material = k_stone;
  u8 pad = 0;
};

struct BridgePlan {
  BridgeForm form = BridgeForm::none;
  // Levels over the bed, mm: the water at the stage, the bankfull level (the lower bank's top),
  // the deck's walking surface and the members' underside over the channel.
  i32 water = 0;
  i32 bankfull = 0;
  i32 deck = 0;
  i32 soffit = 0;
  // The walking surface's ends along x (the approaches meet it there), its width, and the clear
  // span between the supports' inner faces.
  i32 x_start = 0, x_end = 0;
  i32 width = 0;
  i32 span = 0;
  u32 piers = 0;
  bool handrail = false;
  u64 seed = 0;
  Vector<Member> members;
};

// The crossing's numbers in range: a sentence for the first one broken.
bool crossing_in_range(const Crossing& crossing, std::string& why);
// The ground under local x (mm along the path from the centre), world mm: the profile's points
// joined straight, held at its ends. Integer arithmetic, rounded to nearest.
i64 profile_at(const Crossing& crossing, i64 x_mm) noexcept;
// The bankfull depth: the lower of the two banks' tops at the bankfull width's edges, over the bed.
i32 bankfull_depth_mm(const Crossing& crossing) noexcept;
// **The form rule** (bridges.md, "The form").
BridgeForm choose_form(const Crossing& crossing) noexcept;
// A crossing's seed: the layer's mixed with its route, line and station, never its tile.
u64 crossing_seed(u64 layer_seed, const Crossing& crossing) noexcept;
// Solves the crossing's members. False with a sentence for a crossing out of range; a crossing the
// rule leaves a ford is `BridgeForm::none` with no members, and true.
bool plan_bridge(const Crossing& crossing, u64 seed, BridgePlan& out, std::string& why);

// ---- the mesh
// --------------------------------------------------------------------------------------

struct BridgeReport {
  u32 triangles = 0;                        // the finest form's
  u32 kind_triangles[k_member_kinds] = {};  // where they go, by `MemberKind`
  u32 stone_triangles = 0, timber_triangles = 0;
  u32 root_triangles = 0;  // the DAG's coarsest cut: the far form (set by `build_container`)
};
// Every member as a closed, outward-wound solid in the bridge's frame (metres), per material:
// `parts[k_stone]` and `parts[k_timber]`, positions, normals, UVs in metres of surface, indices.
struct MeshPart {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
};
bool solve_bridge(const Bridge& bridge, MeshPart (&parts)[2], BridgePlan& plan,
                  BridgeReport& report, std::string& error);

// The recipe's file: read (refusing an unknown field, a crossing out of range), canonical, and the
// source hash over the canonical form and `k_bridge_version`.
bool read_file(std::string_view text, Bridge& out, std::string& error);
std::string canonical_json(const Bridge& bridge);
u64 source_hash(const Bridge& bridge);
// The container the mesh generator `bridge` builds: the stone and the timber each clustered and
// their DAGs merged, a material each (the layer's roles, from the host's pack).
bool build_container(const Bridge& bridge, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, BridgeReport& report, std::string& error);

// ---- placing it, and its neighbours
// ------------------------------------------------------------------

// The crossing's direction made a unit vector (one correctly rounded square root).
void unit_direction(const Crossing& crossing, f64& ux, f64& uz) noexcept;
// A point of the bridge's frame (x along, z across, mm) in world millimetres, rounded to nearest.
void world_mm(const Crossing& crossing, i64 x_mm, i64 z_mm, i64& wx, i64& wz) noexcept;
// The bridge's instance: at the crossing's centre and bed, turned about y so +x is the path.
scene_gen::Placement place(const Crossing& crossing) noexcept;
// The tile a crossing is realised in: its centre's.
scene_gen::TileCoord tile_of(const Crossing& crossing, i64 tile_mm) noexcept;

// **The approaches** (`engine.scene.TerrainEdit`s of mode `Fill`): for each end of a bridge whose
// deck stands over the ground there, a line from the deck's end out along the path, falling at
// `approach_grade_permille` to the ground, so the walker steps from the tread to the deck with no
// step. A JSON array in the layer's order.
JsonValue approach_edits_json(const Layer& layer, u32 approach_grade_permille);
// An approach's top is the tread or the deck, whichever is wider, and a shoulder of half this each
// side: a crest only a tread wide is two or three points of a 25 to 50 cm ground lattice, which
// rounds it down under the deck's end (the first pictures: a knife-edge ridge meeting the sill, not
// the deck). A choice for the lattice, not a source's number.
inline constexpr i32 k_approach_shoulders_mm = 1000;
i32 approach_width_mm(const Crossing& crossing, const BridgePlan& plan) noexcept;
// How long an approach runs, and the step it leaves at the deck's end against the profile's ground
// with the fill in it (the larger of the two ends), mm: what the tests and the command report.
struct Approach {
  i32 length_mm[2] = {};
  i32 step_mm = 0;
};
Approach approach_of(const Crossing& crossing, const BridgePlan& plan,
                     u32 approach_grade_permille) noexcept;

// **The footprint** (W98's polygon): the bridge's plan, its deck and every support, as four world
// points in whole millimetres (written into `points`, which must outlive the footprint). The
// generator hands it with every tile its disc, grown by `k_shape_reach_mm`, touches: the reach a
// later entry keeps past a shape (the routes' wear, 2 m; scene_gen.md, "The shapes a tile
// occupies"), so a point of a tile gets the whole read's answer. A choice, said so.
inline constexpr i64 k_shape_reach_mm = 2000;
scene_gen::Footprint bridge_footprint(const Crossing& crossing, const BridgePlan& plan,
                                      scene_gen::FootprintPoint (&points)[4]) noexcept;

}  // namespace engine::bridges
