#pragma once

// **The caves capability** (docs/subsystems/caves.md; roadmap G134; ADR-0027): a cave passage from
// a hole in the ground (the terrain's `holes`, W87) in and down, as a mesh — a closed shell wound
// to face inward, open only at the collar that meets the hole's rim — and the speleothems that hang
// and stand in it. A recipe (`engine.caves.Passage`, a scene's `<name>.cave.json`) and a seed in, a
// mesh out, the same bytes on every toolchain and at any job count; the mesh-generator registry
// ("cave") builds it into a `.clusters` container through the derived-data cache as it does a rock,
// and from there it is a mesh like any other: its cluster DAG is its LOD, `scene_collision` cuts
// its collision proxy from that DAG, and the renderer draws and traces it.
//
// The numbers it takes are the grounding's (docs/experiments/caves.md): a phreatic tube twice as
// wide as high, a vadose canyon three times as tall as wide, a keyhole a tube over a canyon; a
// stalactite whose radius grows as its length to the 3/4 (Short, Baygents and Goldstein 2005), a
// soda straw of the drop's bore, a stalagmite of about Curl's minimum diameter.
//
// What it does not own: the hole (the terrain's; `scene_gen::TerrainHoles`), the light inside
// (the renderer's), the collision body (scene_collision's proxy of the mesh). The capability's
// checklist (ADR-0027 decision 2): schema types (schemas/caves.schema), derived data (the mesh
// generator "cave"); no scheduler entry, render pass, protocol method or tunable — a passage is
// content, not a system; LOD the cluster DAG; determinism "derived" (content, never read back into
// a tick); zero cost unused (a scene that names no `.cave.json` never calls it).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <schemas/caves.h>
#include <string>
#include <string_view>

namespace engine::materials {
struct Pack;
}

namespace engine::caves {

inline constexpr std::string_view k_passage_format = "engine.caves.passage";
// Bumped whenever the solver's output for the same recipe changes.
inline constexpr u32 k_caves_version = 1;
// The material role a passage's limestone takes from a pack (materials.md).
inline constexpr std::string_view k_limestone_role = "rock/limestone";

// What a solve made, for a person and the tests.
struct PassageReport {
  u32 rings = 0;
  u32 ring_vertices = 0;
  u32 shell_triangles = 0;
  f32 length_m = 0.0f;  // along the centreline, the shaft included
  u32 drips = 0;        // drip points placed
  u32 stalactites = 0;
  u32 straws = 0;
  u32 stalagmites = 0;
  u32 columns = 0;
  u32 speleothem_triangles = 0;
  u32 drips_over_budget = 0;  // drips the triangle budget left out
  // The collar: its first ring's vertices, in the mesh's frame, and their count.
  u32 collar_first = 0;
  u32 collar_count = 0;
};

// Parses a `.cave.json`'s text and checks every number against its range (the grounding's).
bool read_passage(std::string_view text, Passage& out, std::string& error);
// False, with a sentence naming the field, for a recipe outside its ranges.
bool passage_in_range(const Passage& passage, std::string& error);

// **The solve**: the passage's shell and its speleothems as one mesh in the entrance's frame, one
// primitive and one material (the limestone), positions, normals and UVs (metres along the ring and
// along the passage). Pure and deterministic: products, sums and square roots, the noise an integer
// hash of the seed.
bool solve_passage(const Passage& passage, assets::MeshData& out, PassageReport* report,
                   std::string* error);

// The recipe's identity: its canonical JSON's hash and `k_caves_version`.
u64 passage_source_hash(const Passage& passage);
// Builds the passage into a container: the solve, its cluster DAG (cones on — the shell is wound
// to face inward and the speleothems outward, so every triangle's front is the side a camera in
// the cave sees), its pages, the limestone (a pack's `rock/limestone` set when there is one).
bool build_passage_container(const Passage& passage, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, PassageReport* report,
                             std::string& error, const materials::Pack* pack = nullptr);

// The profile of segment kind `profile` at angle index k of n (k = 0 the side at +x, counter-
// clockwise seen down the passage towards its far end: up at n / 4, the floor at 3n / 4), metres
// across (x, the passage's side) and up (y), the centreline at the origin. For the tests.
Vec2 profile_point(PassageProfile profile, f32 width_m, f32 height_m, u32 k, u32 n) noexcept;
// The height a segment is drawn at: its own, or its profile's ratio of its width.
f32 segment_height(const PassageSegment& segment) noexcept;

// The ideal stalactite's radius at `along` metres from its tip, of length `length` and base radius
// `base` (Short et al. 2005's far field, radius as length^(3/4), from the straw's radius at the
// tip).
f32 stalactite_radius(f32 along, f32 length, f32 base, f32 tip) noexcept;

}  // namespace engine::caves
