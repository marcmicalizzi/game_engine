#pragma once

// structures capability (ADR-0027; docs/plan/14-structures.md; docs/subsystems/structures.md).
//
// A building is a **description** (`engine.structure.Description`, schemas/structures.schema): its
// storeys, its walls as centre lines with openings, its rooms, its stairs, its roofs, its chimneys
// and its plain members, every length whole centimetres in the building's own frame. This module
// owns that type, the one **mesher** that turns a description into geometry by material role, the
// **walk records** beside the mesh (floors, walls, stair flights and openings as plain records, for
// collision, walking tests and validators), and the mesh generator `structure`, which builds a
// `<name>.structure.json` file into a `.clusters` container. A filler (the rural filler, a
// village's) writes descriptions; nothing here writes one but the example below.
//
// What it does not do: lay out rooms or choose openings (the interiors' and the fillers'), place a
// building on a ground, or make a body for collision (scene_collision's, from the mesh's proxy
// and these records).
//
//   [x] schema types      schemas/structures.schema: the description, the recipe, the record
//   [-] scheduler entry   none: a description is meshed at a content build, not ticked
//   [-] render passes     none: its meshes are cluster geometry like any other
//   [x] derived data      the mesh generator `structure` (assets' registry): a recipe's container
//                         in the derived-data cache, keyed by its canonical form and the version
//   [-] protocol methods  none yet (`structure.fill` is the fillers')
//   [-] tunables          none: every number is the description's
//   [x] LOD policy        the mesh's cluster DAG (structures.md, "Forms and the LOD policy")
//   [x] determinism       integer centimetres in, f64 sums, products, one division and a correctly
//                         rounded square root; no C library in a decision; a pinned hash
//   [x] zero cost unused  a scene that names no `.structure.json` never calls it (the registry)
//   [x] docs, tests, size table, bench

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_file.h>

#include <schemas/structures.h>
#include <string>
#include <string_view>

namespace engine::structures {

// The mesher's version, in every recipe's source hash: a change that moves a triangle bumps it
// (plan 14 §14.1: an upgrade may move triangles, never a room or a door).
// 2: a thatch's rounded eave and ridge course, the roof's rafters and collars, the hearth.
// 3: leaves (a door's, a shutter's) and an unglazed window's mullions.
// 4: a wall's batter and wander, rounded corners, a plinth standing proud (a description that
//    states none of them meshes to the same bits as before).
// 5: a thatch's courses and cut eave (the same holds), a leaf's boards each with their own
//    grain, and the example's batter eased to seven eighths.
// 6: a leaf is a mesh of its own (the `leaf` scope), out of the whole, the exterior and the
//    interior (a description with no leaf meshes to the same bits as before).
inline constexpr u32 k_structure_version = 6;

// Determinism stance (ADR-0010): "derived": a mesh is content, never read back into gameplay, and
// the same bytes from every toolchain all the same (structures.md, "Determinism").
inline constexpr const char* k_determinism = "derived";

// ---- limits the validator holds (structures.md, "What a description must satisfy")
inline constexpr i32 k_reach_cm = 1500;  // every coordinate within +-15 m: UVs stay in range
inline constexpr i32 k_min_wall_cm = 5;  // a wall's thickness, 5 cm to 1.5 m
inline constexpr i32 k_max_wall_cm = 150;
inline constexpr i32 k_min_pier_cm = 15;  // wall left between an opening and its wall's end
inline constexpr u32 k_max_storeys = 4;
inline constexpr u32 k_max_walls = 256;
inline constexpr u32 k_max_openings = 256;

// A door's threshold stands this far above its sill, and is what its reveal's foot is drawn at
// (millimetres; structures.md, "Openings").
inline constexpr i32 k_threshold_mm = 20;

// The surfaces, one material each: `structure::Surface`'s values index every per-surface array.
inline constexpr u32 k_surfaces = 9;

// The role a surface is drawn with (`Description::roles`), and what stands behind it when the
// host's pack has no such set: another role (empty: none) and a flat colour.
std::string_view role_of(const structure::Description& d, structure::Surface s) noexcept;
std::string_view fallback_role(structure::Surface s) noexcept;
Vec4 fallback_colour(structure::Surface s) noexcept;

// ---- the mesh ----------------------------------------------------------------------------------

// One surface's triangles in the building's frame, metres: positions, normals, UVs in metres of
// surface (a wall's u along it and v up; a floor's x and z; a roof's u along the ridge and v up the
// slope; a timber's u along its grain), indices.
struct MeshPart {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
};

// ---- the walk records: what collision and walking need, beside the mesh
// ----------------------------
//
// Plain records in the building's frame, metres, made from the description alone (no mesh is
// read): a consumer transforms them by the building's placement. `storey`, `wall`, `opening`,
// `stair` and `room` index the description's arrays (`room` is a `Room.id`; -1 outside or none).

// A floor a walker stands on: a storey's floor at `y` over `outline[first .. first + count)`.
struct WalkFloor {
  u32 storey = 0;
  f32 y = 0;
  u32 first = 0;
  u32 count = 0;
};
// A hole in storey `storey`'s floor (a stair's well), x0 z0 x1 z1.
struct WalkWell {
  u32 storey = 0;
  u32 stair = 0;
  f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
};
// A wall: its centre line, its thickness and where it stands from and to (its lowest top).
struct WalkWall {
  u32 wall = 0;
  Vec2 from, to;  // x and z
  f32 thickness = 0;
  f32 y0 = 0, y1 = 0;
};
// An opening through a wall: the two jambs' feet on the wall's centre line (`a` nearer the wall's
// `from`), the sill and the head, the wall's thickness (the passage's depth) and the rooms either
// side of it (`room_a` on the left of the wall's `from` to `to`).
struct WalkOpening {
  u32 opening = 0;
  u32 wall = 0;
  structure::OpeningKind kind = structure::OpeningKind::door;
  Vec2 a, b;  // x and z
  f32 sill = 0, head = 0;
  f32 thickness = 0;
  i32 room_a = -1, room_b = -1;
};
// One straight run of steps: the middle of its first riser's foot, the direction it climbs (a
// unit vector on the ground), and its steps. Its last riser lands on `top` (the floor, or a
// landing).
struct WalkFlight {
  u32 stair = 0;
  Vec3 foot;
  Vec2 dir;  // x and z
  f32 width = 0;
  f32 rise = 0;
  f32 going = 0;
  u32 risers = 0;
  f32 top = 0;
};
// A plain member that stops a walker (`Box::solid`): its low and high corners.
struct WalkBox {
  u32 box = 0;
  Vec3 min, max;
};
// A leaf as it stands in opening `opening` (`Opening::leaf`): a plumb slab `thickness` deep about
// the line from `hinge` to `tip` (x and z), from `y0` to `y1`, `open_deg` degrees from shut. A
// shut leaf closes its opening; one open a quarter turn lies beside its hinge jamb, in a thick
// wall's reveal. The opening's own record is the same with or without a leaf.
struct WalkLeaf {
  u32 opening = 0;
  Vec2 hinge, tip;
  f32 y0 = 0, y1 = 0;
  f32 thickness = 0;
  i32 open_deg = 0;
};
struct Walk {
  Vector<WalkFloor> floors;
  Vector<Vec2> outline;  // the floors' outlines, x and z
  Vector<WalkWell> wells;
  Vector<WalkWall> walls;
  Vector<WalkOpening> openings;
  Vector<WalkFlight> flights;
  Vector<WalkBox> boxes;
  Vector<WalkLeaf> leaves;
};

// ---- the rooms' graph
// ----------------------------------------------------------------------------

// Two rooms joined: by an opening (a door or a doorway, never a window) in a wall that names both,
// or by a stair. `a` or `b` is -1 for the outside: such a link is an entrance.
struct RoomLink {
  i32 a = -1;
  i32 b = -1;
  u32 index = 0;  // the opening's, or the stair's
  bool stair = false;
};
void room_links(const structure::Description& d, Vector<RoomLink>& out);
// True when every room is reached from the outside through the links (a description with no rooms
// is reachable); `unreached` (nullable) takes the first room that is not.
bool rooms_reachable(const structure::Description& d, i32* unreached = nullptr);

// ---- the mesher
// ----------------------------------------------------------------------------------

// What the mesher counted (structures.md, "Budgets").
struct Report {
  u32 triangles = 0;
  u32 surface_triangles[k_surfaces] = {};
  u32 root_triangles = 0;  // the DAG's coarsest cut: the far form (set by `build_container`)
  u32 clusters = 0;        // (set by `build_container`)
};

// What a description must satisfy to be meshed (structures.md, "What a description must
// satisfy"): indices that name something, lengths in range, storeys that stack, straight walls,
// every opening inside its wall with a pier each side and under its wall's top, no two openings of
// a wall overlapping, a roof over a rectangle. False with a sentence naming the rule, the element
// and what was measured. Nothing is repaired.
bool validate(const structure::Description& d, std::string& why);

// The walk records of a valid description.
bool walk_records(const structure::Description& d, Walk& out, std::string& error);

// The mesh by surface: `parts[static_cast<u32>(structure::Surface::x)]`. `scope` picks the whole
// building, or what is seen from outside, or the rest (structures.md, "Exterior and interior"),
// none of which holds a leaf; or the `leaf` scope, leaf `leaf` alone (`door_records`' order),
// shut, its hinge's foot at the origin and its free edge along +x (structures.md, "A leaf is a
// mesh of its own"). False for a leaf the description does not have.
bool solve(const structure::Description& d, structure::MeshScope scope,
           MeshPart (&parts)[k_surfaces], Report& report, std::string& error, u32 leaf = 0);

// ---- the recipe file and the container
// -----------------------------------------------------------

// A `<name>.structure.json` file: read (refusing an unknown field and an invalid description),
// canonical, and the source hash over the canonical form and `k_structure_version`.
bool read_file(std::string_view text, structure::Recipe& out, std::string& error);
std::string canonical_json(const structure::Recipe& recipe);
u64 source_hash(const structure::Recipe& recipe);

// The leaves of `d` that turn on a hinge — a door's, a casement's, a shutter's — as the container's
// section 53 records them (structures.md, "Leaves and mullions"; geometry.md, "Doors"): shut, with
// the way each
// swings and the angle the description stands it at, in metres in the building's frame. Every
// leaf the mesher hangs, in the openings' order and then hinge first; empty for a description
// with none. `build_container` writes them into every container of the whole or the exterior
// it builds (the leaf's own mesh records none).
void door_records(const structure::Description& d, Vector<geometry::ClusterFileDoor>& out);

// **A leaf's own recipe** (structures.md, "A leaf is a mesh of its own"): `house` with the
// `leaf` scope and leaf `index`, canonical, and the file name a host hands it under
// (`<stem>.leaf<index>.structure.json` beside the house's own name), so a tile that hangs two
// houses of one description draws one leaf mesh for both.
std::string leaf_recipe(const structure::Recipe& house, u32 index);
std::string leaf_path(std::string_view house_path, u32 index);

// The container the mesh generator `structure` builds: each surface's triangles clustered and the
// DAGs merged, a material a surface, its set the description's role from the host's pack through
// the tile contract (the container names its images and carries their texture records).
bool build_container(const structure::Recipe& recipe, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error);

// ---- an example
// ----------------------------------------------------------------------------------

// A one-room cot: `width` by `depth` centimetres inside a wall `wall` thick (an even number, so
// the roof's rectangle is the walls' outer faces in whole centimetres), one storey open to a
// thatched gable roof, a door and a window in the front wall (low z), a window in the back, a
// hearth and its stack on the low-x gable. What the tests, the bench and the page's pictures mesh,
// and a starting point for a filler's tests (structures.md, "The example").
structure::Description example_cot(i32 width = 600, i32 depth = 400, i32 wall = 60);

}  // namespace engine::structures
