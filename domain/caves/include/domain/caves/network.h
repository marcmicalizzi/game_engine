#pragma once

// **Cave networks** (docs/subsystems/caves.md, "The network"; roadmap G146; the grounding,
// docs/experiments/cave-networks.md): a cave a walker enters and explores, as a solver's output.
//
// Three things, in the order they are made:
//
// 1. **The plan** (`plan_network`): one deterministic graph for the whole network, built once and
//    held whole — strands of stations a metre apart laid out in the host bed's own coordinates
//    (trunks along strike where the soluble member meets each base level, feeders and connectors
//    along dip, a bypass, crawls, halls), every strand tried against the ground it is under: it
//    ends in a choke where the cover runs out, runs on into daylight where the ground falls away
//    (a mouth, found), and a lead follows the bed to the nearest daylight or climbs a shaft. A
//    hall under too little cover has fallen in: a fissure. The plan is whole millimetres.
// 2. **Cells** (`cell_recipe`, `solve_cell`, the mesh generator "cavecell"): the network's one
//    closed surface — the zero of a field sampled on the world's lattice and meshed by surface
//    nets — cut into cells of at most `cell_length_m` of passage, each a mesh of its own with its
//    own cluster DAG. A cell's recipe is everything its surface is a function of, so it is built
//    with no plan and no ground in hand, and two cells make the same vertex in a lattice cube they
//    share: they meet at a portal with no gap.
// 3. **The placement generator "caves"** (`network_placement.cpp`): `open` plans, writes the
//    recipes and names them; `tile` places the cells whose bounds' middle is on the tile.
//
// What a plan is a function of: the entry's parameters and the ground (`scene_gen::Ground`),
// asked at whole millimetres. Nothing else; not the tile that asked, not a thread count.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/scene_gen/scene_gen.h>

#include <memory>
#include <schemas/caves.h>
#include <string>
#include <string_view>

namespace engine::caves {

inline constexpr std::string_view k_network_format = "engine.caves.network";
inline constexpr std::string_view k_cell_format = "engine.caves.cell";
// Bumped whenever a plan or a cell's surface changes for the same inputs, so a cached cell is
// never an older solve's.
inline constexpr u32 k_network_version = 1;
// The roles a network's surfaces take from a pack (materials.md); `rock/limestone` is caves.h's.
inline constexpr std::string_view k_sediment_role = "cave/sediment";
inline constexpr std::string_view k_breakdown_role = "cave/breakdown";
inline constexpr std::string_view k_calcite_role = "cave/calcite";

enum class StrandKind : u8 {
  trunk = 0,  // a phreatic tube along strike at a level
  feeder,     // a vadose canyon down dip into a trunk
  connector,  // the route water took from a level down to the next
  bypass,     // a flood loop round a reach of a trunk
  crawl,      // a tube too low to walk, pinching out
  lead,       // a tube along the bed to daylight
  shaft,      // a pit climbing to the ground
  fissure,    // the gap over a hall that has fallen in
  sump,       // a trunk's end under the water table
};
const char* strand_name(StrandKind kind) noexcept;

enum class EndKind : u8 { junction = 0, choke, mouth, pinch, sump, hall };
const char* end_name(EndKind kind) noexcept;

// A station of the plan: `station` is the recipe's own record (whole millimetres), and beside it
// what only the plan needs.
struct PlanStation {
  CellStation station;
  i32 cover_mm = 0;       // the ground over the roof here (negative: the roof is out)
  i32 clear_h_mm = 0;     // floor to roof
  i32 clear_w_mm = 0;     // the width a walker's shoulders have
  bool passable = false;  // the walker's capsule fits here
};

struct PlanEdge {
  u32 first = 0;  // into `Plan::stations`
  u32 count = 0;
  u32 node_a = 0, node_b = 0;
  StrandKind kind = StrandKind::trunk;
  u8 level = 0;
  bool walkable = false;  // every station passes the capsule and the slope is walked
  f32 length_m = 0.0f;
};

struct PlanNode {
  i64 x = 0, y = 0, z = 0;
  u32 degree = 0;
  EndKind end = EndKind::junction;  // what a node of degree one is
};

struct PlanHall {
  CellHall hall;
  f32 cover_m = 0.0f;  // the ground over the dome's top
  f32 span_m = 0.0f;
  bool fallen = false;  // under `collapse_cover` of its span: it has a fissure
};

// One point of an outline: whole millimetres of the world, on the ground.
struct OutlinePoint {
  i64 x = 0, y = 0, z = 0;
};

// **A mouth**: where the void meets the ground. Found, not declared.
struct PlanMouth {
  u32 edge = 0;
  i64 x = 0, y = 0, z = 0;  // on the ground, the middle of the opening
  // The opening itself: the ground's points inside the void, a closed polygon on the ground.
  Vector<OutlinePoint> outline;
  // What the terrain's hole must be for it (docs/subsystems/caves.md, "What the terrain's hole
  // must be"): the outline grown by the hole's cell, so no tooth of the ground stands in the
  // opening, inside the apron the mesh owns.
  Vector<OutlinePoint> hole;
  // The ground the mouth's cells are cut by: a window of the lattice's columns.
  i64 patch_i = 0, patch_j = 0;
  u32 patch_nx = 0, patch_nz = 0;
  Vector<i32> patch;        // millimetres from the anchor's height
  f32 area_m2 = 0.0f;       // the opening's, in plan
  f32 slope = 0.0f;         // the ground's across it, rise over run
  f32 plan_depth_m = 0.0f;  // the outline's least width in plan: what a hole's cells must fit in
  bool pit = false;
};

// A portal: where two cells meet. What a host that loads by what can be seen needs of it.
struct PlanPortal {
  u32 cell_a = 0, cell_b = 0;  // `k_outside` for the open air at a mouth
  i64 x = 0, y = 0, z = 0;
  Vec3 normal{0.0f, 0.0f, 1.0f};  // from a to b
  f32 half_width_m = 0.0f, half_height_m = 0.0f;
};
inline constexpr u32 k_outside = 0xFFFFFFFFu;

struct PlanCell {
  // Inclusive lattice indices from the anchor.
  i64 lo[3] = {};
  i64 hi[3] = {};
  scene_gen::TileCoord tile{};
  f32 passage_m = 0.0f;
  bool mouth = false;  // it holds a piece of a mouth: placed in every ring
  bool hall = false;
  bool flooded = false;  // every roof in it is under the water table: no surface, so no mesh
  u32 stations = 0;
};

struct PlanReport {
  u32 levels = 0;
  u32 strands = 0;
  u32 loops = 0;         // independent cycles of the graph
  u32 dead_ends = 0;     // nodes of degree one that are not mouths
  u32 mouths_found = 0;  // the ground's own
  u32 mouths_led = 0;    // a lead's or a shaft's
  u32 fissures = 0;
  f32 passage_m = 0.0f;
  f32 walkable_m = 0.0f;
  f32 reachable_m = 0.0f;  // walkable passage a walker reaches from a mouth
  f64 base_level_m = 0.0;
  f64 member_base_m = 0.0, member_top_m = 0.0;  // the host, bed coordinate
  f64 least_margin_m = 0.0;  // the least rock between a passage and the member's top or base
  f64 dip_offset_m = 0.0;    // how far down dip of `origin` the lowest level lies
  std::string notes;         // what was dropped, and why, a sentence each
};

struct Plan {
  Network network;
  scene::Formation formation;  // the network's, or the level default
  // The anchor: a point of the world's lattice near `origin`, whole millimetres.
  i64 anchor[3] = {};
  i64 voxel_mm = 250;
  i64 tile_mm = 32000;
  bool has_water = false;
  i64 water_mm = 0;
  Vector<PlanStation> stations;
  Vector<PlanEdge> edges;
  Vector<PlanNode> nodes;
  Vector<PlanHall> halls;
  Vector<PlanMouth> mouths;
  Vector<PlanPortal> portals;
  Vector<PlanCell> cells;
  PlanReport report;
};

// Parses a network's JSON text and checks its ranges.
bool read_network(std::string_view text, Network& out, std::string& error);
bool network_in_range(const Network& network, std::string& error);

// **The plan.** False, with a sentence, for parameters out of range or a member no level fits.
bool plan_network(const Network& network, const scene_gen::Ground& ground, i64 tile_mm, Plan& out,
                  std::string& error);
// `hash_bytes` over every number of the plan in order: what the tests pin.
u64 plan_hash(const Plan& plan);

// The bed coordinate (the formation's, metres) at a point of the world given in whole
// millimetres: what the tests hold the passages inside the member with.
f64 bed_coordinate(const Plan& plan, i64 x_mm, i64 y_mm, i64 z_mm);

// The void's field at points of the world (whole millimetres), no ground: negative inside the
// cave, with its floors, its water and its walls' detail. Built once from a plan, asked often: the
// tests' and a host's question "is this point in the cave".
class PlanField {
 public:
  explicit PlanField(const Plan& plan);
  ~PlanField();
  PlanField(const PlanField&) = delete;
  PlanField& operator=(const PlanField&) = delete;
  f64 void_at(i64 x_mm, i64 y_mm, i64 z_mm) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A plan view and a long section (elevation against distance along strike), as SVG text: what a
// person lays beside a published survey.
std::string plan_svg(const Plan& plan);
std::string section_svg(const Plan& plan, const scene_gen::Ground& ground);
// The mouths' holes as `engine.scene.TerrainHole` entries, a JSON array: what a scene's terrain
// takes today.
std::string holes_json(const Plan& plan);
// Where things are, for whoever dresses the scene (a light in the hall, a camera at a mouth): the
// mouths with the way out of each, the halls, and every edge's floor under its centreline every
// fourth station. Metres of the world.
std::string survey_json(const Plan& plan);

// ---- cells --------------------------------------------------------------------------------------

// What a cell's solve made.
struct CellReport {
  u32 lattice_points = 0;
  u32 vertices = 0;
  u32 triangles = 0;
  u32 class_triangles[5] = {};  // rock, sediment, breakdown, water, apron
  u32 drips = 0, stalactites = 0, stalagmites = 0, columns = 0, flowstones = 0;
  // The mesh's origin, whole millimetres of the world: where its instance stands.
  i64 origin[3] = {};
};

// Cell `cell`'s recipe: `part` 0 its rock, 1 its dressing.
void cell_recipe(const Plan& plan, u32 cell, u32 part, CellRecipe& out);
bool read_cell_recipe(std::string_view text, CellRecipe& out, std::string& error);
std::string cell_recipe_json(const CellRecipe& recipe);
u64 cell_source_hash(const CellRecipe& recipe);

// **The solve**: the cell's mesh in its own frame (the origin its lattice's low corner), one
// primitive a surface class with a material each. Vertices lie on a 1/1024 m grid of the world,
// so a vertex two cells share is one point exactly.
bool solve_cell(const CellRecipe& recipe, assets::MeshData& out, CellReport* report,
                std::string& error);
// The mesh's boundary vertices (an edge with one triangle): the portal rings and an apron's edge,
// which every cut of the cell's DAG keeps.
void boundary_vertices(const assets::MeshData& mesh, Vector<u8>& locked);

bool build_cell_container(const CellRecipe& recipe, const assets::MeshGeneratorInputs& inputs,
                          const geometry::ClusterLodOptions& lod, u32 page_bytes,
                          geometry::ClusterFileData& out, CellReport* report, std::string& error);

// Where a cell's instance stands.
WorldPos cell_origin(const Plan& plan, u32 cell) noexcept;

}  // namespace engine::caves
