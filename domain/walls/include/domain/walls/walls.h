#pragma once

// Dry-stone walls along a line (roadmap G145; docs/subsystems/walls.md, the sources and numbers
// in docs/experiments/dry-stone-walls.md).
//
// **What it is.** Two generators in one capability. The mesh generator `wall` solves one module —
// a stretch of wall a few metres long with every face stone, pinning, through-stone, cover stone
// and cope stone as geometry, laid course by course with level beds, joints crossed and the courses
// graded from large at the foot to small under the cover — from a recipe (`engine.walls.Module`,
// `<name>.wall.json`), and carries two authored coarser forms in the same DAG: the mid form (the
// courses as bands, the cope as blocks) and the outline. The placement generator `walls` reads a
// layer of lines (`engine.walls.Layer`), plans every line once — its stations, its runs between
// vertices and gateways, which module stands at each station, which stretches have fallen — and a
// tile realises the modules whose middle falls in it, standing each on the ground asked at the
// module's own integer millimetres. The far rings draw a strip a piece of a few modules.
//
// **Why modules and not a mesh a tile.** A placement generator names its meshes once, when the
// scene is read (`PlacementGeneratorDesc::meshes`; scene_gen.md, "The two kinds"); a tile only
// places instances of them. So a wall reaches the renderer as a small kit of solved modules
// (39 meshes at two variants, resident whatever the world's size) placed by station, a module an
// instance: a hundred metres of wall is about 34 instances, against the proof of concept's 1,900.
//
// **What it is not.** No ground edit (a wall sits on the ground; a terrace's cut is W93's seam),
// no fences or retaining walls, no lithology read from the formation (a stone colour palette of
// a slabby metamorphic stone), no moss or lichen (the cover solver's), no collision of its own.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/walls.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::walls {

// ---- the module solver -----------------------------------------------------------------------

// The solver's version, in every recipe's source hash: a change that moves a stone bumps it.
inline constexpr u32 k_wall_version = 3;

// What a stone is in a solved module (the report's, for the tests and the page's counts).
enum class StoneRole : u8 { face, footing, through, tie, pinning, cover, cope, rubble, core };

// One stone of a solved module, in the module's frame (x along the wall, y up, z across; the
// module's middle at the origin, its foot line y = grade * x): its bed's and top's level, its
// extent along the wall, the face it belongs to (+1 the +z face, -1 the -z face, 0 both or none),
// the sweep (the course) it was laid in, its role. `z0`/`z1` are its extent across the wall.
struct Stone {
  f32 x0 = 0, x1 = 0;
  f32 bed = 0, top = 0;
  f32 z0 = 0, z1 = 0;
  u16 sweep = 0;
  i8 side = 0;
  StoneRole role = StoneRole::face;
};

struct ModuleReport {
  Vector<Stone> stones;
  // The authored coarser forms over the same positions: the mid form and the outline.
  Vector<u32> mid_indices;
  Vector<u32> far_indices;
  u32 near_triangles = 0;
  u32 mid_triangles = 0;
  u32 far_triangles = 0;
  // Where the finest form's triangles go, indexed by `StoneRole` (walls.md, "Budgets").
  u32 role_triangles[9] = {};
  // The seams (joints through three courses) the solver crossed by moving a joint along its
  // course, and those it left (walls.md, "The module").
  u32 seams_crossed = 0;
  u32 seams_left = 0;
  // The stone's volume in the faces, the throughs, the cover and the cope, m^3, and its length.
  f64 stone_volume_m3 = 0;
  f64 length_m = 0;
};

// The ranges docs/experiments/dry-stone-walls.md pins, as a sentence for the first one broken.
bool module_in_range(const Module& module, std::string& why);
// The batter of each face as run over rise (0.156 = 1:6.4): what the two widths and the body's
// height give.
f64 batter(const Module& module) noexcept;
// The body's height: ground to the cover course's bed, metres.
f64 body_height(const Module& module) noexcept;
// The through-stones' row: their bed above the ground, metres, and their nominal spacing.
inline constexpr f64 k_through_low_m = 0.50;
inline constexpr f64 k_through_high_m = 0.70;
// The most a module's row is spaced at before its stretch to a run's fit: as many throughs as
// keep them under 0.80 m, evenly, half a spacing in from each end, each drawn within 3 cm (0.75 m
// in a 3 m module), so a run stretched to 118 % still keeps them under TCV's 1 m centres.
inline constexpr f64 k_through_spacing_m = 0.80;
// The footing's projection beyond each face (TCV's scarcement, 50 to 75 mm).
inline constexpr f64 k_scarcement_m = 0.06;
// A cope stone's thickness along the wall, metres: mostly thin slabs, now and then a block
// (TCV, Types of coping: random coping; slab copes 50 to 200 mm thick).
inline constexpr f64 k_cope_thin_m = 0.04;
inline constexpr f64 k_cope_thick_m = 0.16;

// Solves a module: the finest form (every stone) in `out` as positions, normals, UVs and indices,
// one material and its stone palette image, and the coarser forms' indices in the report.
bool solve_module(const Module& module, assets::MeshData& out, ModuleReport& report,
                  std::string& error);

// The recipe's file: read (refusing an unknown field, a value out of range), canonical, and the
// source hash over the canonical form and `k_wall_version`.
bool read_file(std::string_view text, Module& out, std::string& error);
std::string canonical_json(const Module& module);
u64 source_hash(const Module& module);

// The container the mesh generator `wall` builds: the module's DAG with the mid form and the
// outline as authored levels at `k_mid_error_m` and `k_far_error_m`.
inline constexpr f32 k_mid_error_m = 0.02f;
inline constexpr f32 k_far_error_m = 0.08f;
bool build_container(const Module& module, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, ModuleReport& report, std::string& error);

// ---- the plan: stations along a line
// -------------------------------------------------------------

// The grade classes a kit is solved at, thousandths: 0, 125, ... 500 (and their negatives for the
// signed kinds). A module stands on the class nearest its ground's grade and is pitched by the
// rest.
inline constexpr i32 k_grade_step_permille = 125;
inline constexpr i32 k_grade_classes = 4;  // each side of 0
inline constexpr f64 k_max_grade = 0.5;

// One module of a line's plan: where it stands (its start and end stations, whole centimetres of
// arc length, and the integer millimetres of both ends, lengthened at a corner by its mitre), what
// it is and its variant. `reversed` for a head or a broken stretch whose wall end lies at its high
// station: its mesh is turned end for end.
struct Slot {
  i64 s0_cm = 0, s1_cm = 0;
  i64 x0_mm = 0, z0_mm = 0, x1_mm = 0, z1_mm = 0;
  u32 id = 0;  // stable within the layer, from 1
  u32 run = 0;
  u32 line = 0;
  ModuleKind kind = ModuleKind::run;
  u8 variant = 0;
  bool head_at_end = false;  // a head, or a broken stretch's standing end, at the high station
  u8 pad = 0;
};

// A piece of the far form: a strip over consecutive standing modules of one run.
struct Piece {
  i64 x0_mm = 0, z0_mm = 0, x1_mm = 0, z1_mm = 0;
  u32 id = 0;
  u32 pad = 0;
};

struct LinePlan {
  Vector<i64> x_mm, z_mm;  // the vertices, a closed line's first repeated last
  Vector<i64> station_cm;  // per vertex
  i64 length_cm = 0;
  u32 first_slot = 0, slot_count = 0;
  u32 eligible = 0, fallen = 0;  // the stretches a ruin may take, and how many it took
};

// Which tile a slot or a piece is realised in: the tile its middle's integer millimetres fall in.
struct TileRef {
  i32 x = 0, z = 0;
  u32 index = 0;
  u32 pad = 0;
};

struct Plan {
  Vector<LinePlan> lines;
  Vector<Slot> slots;    // every line's, in order
  Vector<Piece> pieces;  // the far strips
  i64 tile_mm = 32000;
  // Sorted by tile (z, then x), then index: which a tile realises.
  Vector<TileRef> slot_tiles;
  Vector<TileRef> piece_tiles;
};

// The tile a point in whole millimetres falls in.
scene_gen::TileCoord tile_of(i64 x_mm, i64 z_mm, i64 tile_mm) noexcept;
// The range of `refs` on a tile.
std::span<const TileRef> refs_on(const Vector<TileRef>& refs, scene_gen::TileCoord tile) noexcept;

// Plans every line of the layer: stations, runs, slots, the ruin's fallen stretches, the far
// pieces and the tile index. A function of the layer alone (and the tile size, for the index).
bool make_plan(const Layer& layer, i64 tile_mm, Plan& out, std::string& why);

// The slot standing at a station of a line (the one whose [s0, s1) holds it), or null for a
// station in a gateway or past the line's end.
const Slot* slot_at(const Plan& plan, u32 line, i64 station_cm) noexcept;

// The integer millimetres of a station on a line.
void point_at(const LinePlan& line, i64 station_cm, i64& x_mm, i64& z_mm) noexcept;

// The kit's mesh order: run and fallen by variant and unsigned class, head and broken by signed
// class, the strip last.
u32 kit_size(u32 variants) noexcept;
u32 kit_index(ModuleKind kind, u32 variant, i32 grade_class, u32 variants) noexcept;
// The recipe of each kit entry, in kit order.
void kit_recipes(const Layer& layer, Vector<Module>& out);

// What a slot's module is once it stands on the ground: the kit entry, its placement, and what
// the tests read (its grade, the class it took, the pitch left over, the depth it was sunk by).
struct Standing {
  scene_gen::Placement placement;
  f64 grade = 0;
  i32 grade_class = 0;
  f64 sink_m = 0;
};
// Stands a slot on the ground: the ground asked at its ends' and its feet's integer millimetres.
Standing stand_slot(const Plan& plan, const Layer& layer, const Slot& slot,
                    const scene_gen::Ground& ground) noexcept;
scene_gen::Placement stand_piece(const Plan& plan, const Layer& layer, const Piece& piece,
                                 const scene_gen::Ground& ground) noexcept;

}  // namespace engine::walls
