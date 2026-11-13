#pragma once

// Static, terrain-aware rockfall and sediment authoring (G128; docs/subsystems/rocks.md), with the
// cones under gullies, the blocks shed below the faces and the formation's lithology (G143).
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>

namespace engine::rocks {
// 2 since G143 (2026-11-10): a bed's bare rim on steep ground sinks below the face it meets.
// 3 (G143, second agent): the bed's outline, its sink and its normals come from the plan.
// 4 (G143's third pass): the bed's top is rubble, its clasts anchored to the world, the loose
// fragments of the largest band alone, the toe's clasts spread over metres. 5: the toe's clasts on
// the gentle ground only (a mesh built at 4 may carry clasts on the face). 6 (T108): a loose stone
// is its own primitive and material, its rock's set at that set's repeat (rocks.md, "A stone wears
// its rock"). 7 (G183): a foot's supply spreads along the wall through a kernel whose wedges are a
// cone's flanks (rocks.md, "Cones that climb their gullies"). 8 (G182): the bed's clasts are
// angular, tilted blocks of several heights over hollows, drawn at 10 cm (rocks.md, "The bed is
// rubble"). 9 (G190): a face's fall comes to rest on ground gentler than the repose, and a pile
// that would end in a cut is moved on down or refused (rocks.md, "Where a pile comes to rest").
// 10 (G190): a face a ground edit cut (a channel's bank) sheds nothing. 11 (G191): a patch whose
// finest form is over `k_deposit_piece_triangles` is handed as pieces, and a container over
// `k_deposit_clusters_max` is built again at half its budget.
inline constexpr u32 k_deposit_version = 11;
// G191 (rocks.md, "A patch under the cluster limit"): the most clusters a patch's container holds,
// the streamed host's `renderer.meshes.max_clusters` default, which refuses a larger tile mesh at
// the walk. `build_deposit_container` holds it.
inline constexpr u32 k_deposit_clusters_max = 4096;
// G191: the most triangles a piece's finest form holds (`deposit_patch_triangles`), so a patch at
// the forest's density is handed as pieces each far under the cluster limit (rocks.md, "A patch
// under the cluster limit", has the measured density it is set from).
inline constexpr u64 k_deposit_piece_triangles = 40000;
// The bed's edge (rocks.md, "The edge and the seams"): inside its outline toward bare gentle ground
// a deposit is no thicker than `k_edge_slope` (tan 31 degrees, the toe's own slope) times its
// distance in, less half a spacing; past it the piles' surfaces continue under the ground; and the
// first ring of samples either side is moved by world-anchored noise of amplitude `k_edge_amp` on a
// `k_edge_lattice_mm` lattice, so the outline falls between the samples.
inline constexpr f64 k_edge_slope = 0.6;
inline constexpr f64 k_edge_amp = 0.1;
inline constexpr i64 k_edge_lattice_mm = 2000;

// What a face shed onto one foot sample (G143; rocks.md, "What a face sheds"): the face area
// routed to the foot times the layer's supply, stood as a pile at the repose angle against the face
// whose apex is `height_m` over the foot. Under a gully it is a cone; along a planar face the piles
// of neighbouring feet are one sheet. World millimetres and metres.
struct DepositPile {
  i64 x_mm = 0;  // the apex, on the face above the foot
  i64 z_mm = 0;
  f64 apex_m = 0;     // the apex's world height: the foot's ground plus `height_m`
  f64 height_m = 0;   // the apex over the foot's ground
  f64 face_m2 = 0;    // the face area routed to the foot (surface area, not plan area)
  f64 volume_m3 = 0;  // what it was asked to hold: `face_m2` times the supply
  f64 held_m3 = 0;    // what the part of the deposit it won holds
  f64 reach_m = 0;    // the farthest sample it covers, horizontally from the apex
  u32 foot = 0;       // the foot's sample index
  u32 source = 0;     // the apex's sample, the face whose bed it is of
};

// A block of the plan, in world millimetres and metres: shed below a face, or one of a cone's.
struct DepositPlanBlock {
  i64 x_mm = 0;
  i64 z_mm = 0;
  f64 y_m = 0;        // the deposit's top (or the ground) under its centre
  f64 half[3] = {};   // x and z across, y its thickness
  f64 slope[2] = {};  // the top's gradient under it
  f64 yaw = 0;
  f64 buried = 0;
  f64 distance_m = 0;  // travelled past the top of its talus, along the ground
  i64 top_x_mm = 0;    // the top of the talus it was shed onto
  i64 top_z_mm = 0;
  f64 top_m = 0;
  u64 seed = 0;
  RockType type = RockType::Granite;
};

// What a plan refused to stand (G190; rocks.md, "Where a pile comes to rest"): a pile that had
// nowhere further down its fall line to rest, or still ended in a cut after its moves, or a face's
// fall that left the region. World millimetres of the foot (or of the border sample it left by).
struct DepositRefusal {
  i64 x_mm = 0;
  i64 z_mm = 0;
  f64 volume_m3 = 0;  // what it was asked to hold
  f64 cut_m = 0;      // how far its surface stood over the bare ground past it; 0: left the region
};

struct DepositPlan {
  u32 width = 0;
  u32 depth = 0;
  i64 x_mm = 0;
  i64 z_mm = 0;
  i64 spacing_mm = 0;
  Vector<f64> ground;
  Vector<f32> thickness;
  Vector<f32> coarse;
  Vector<f64> drainage_m2;
  // G143: every foot's pile that won a sample, the blocks, and (with a formation) the rock.
  Vector<DepositPile> piles;
  Vector<DepositPlanBlock> blocks;
  Vector<RockType> rock;  // per sample, the bed at the face above it (layer has a formation)
  // G143's edge and seams: per sample, a bare top's depth below the ground, and the top's
  // gradient (x then z, two a sample) by central differences over the plan.
  Vector<f32> sink;
  Vector<f32> slope;
  // G190: the piles refused, how many were moved on down their fall lines, the rounds the flood
  // ran, and the most any kept pile's surface stood over the bare ground past its outline (at most
  // the cut bound, 0.22 m, unless the rounds ran out).
  Vector<DepositRefusal> refused;
  u32 moved = 0;
  u32 rounds = 0;
  f64 worst_cut_m = 0;
};

// Pure finite-window solve; no I/O, persistent state or dependence on tile arrival order.
// Refuses absent/nonfinite ground, incompatible grids and exceeded budgets. Output clears on error.
// `uncut` (G190; rocks.md, "Where a pile comes to rest"): the ground before its edits
// (`scene_gen::Context::uncut()`), when it differs from `ground`. A sample sheds only where both
// are steep, so a channel's cut bank or a bench's riser, a face an edit made, sheds nothing. Null:
// the ground has no edits.
bool plan_deposits(const DepositLayer& layer, const scene_gen::Ground& ground, DepositPlan& out,
                   std::string& error, const scene_gen::Ground* uncut = nullptr);
// The plan's ground converges here (G143): the contour curvature over `scale_samples` samples,
// per metre, positive in a hollow; zero on a face whose contours are straight.
f64 deposit_convergence(const DepositPlan& plan, u32 i, u32 scale_samples);
bool deposit_patch_in_range(const DepositPatch& patch, std::string& error);
// The mesh has two primitives (T108; rocks.md, "A stone wears its rock"): the bed, material 0, its
// UVs in repeats of `patch.material.tile_m`; and the loose stones (the fragments, the toe's clasts
// and the shed blocks), material 1, charted in repeats of `stone_tile_m` (0: the bed's), because a
// stone wears its rock's own set and not the bed's. A patch with no stone has the bed's alone.
bool solve_deposit(const DepositPatch& patch, assets::MeshData& out, std::string& error,
                   f64 stone_tile_m = 0);
// Every refusal `solve_deposit` would make, without building the mesh (G143; rocks.md, "A tile
// solves"): what the generator's `open` checks each patch with, so it solves none.
bool deposit_patch_fits(const DepositPatch& patch, std::string& error);
// G191 (rocks.md, "A patch under the cluster limit"): the most triangles `solve_deposit` draws the
// patch's finest form with (its bed at the steps the budget holds, every loose fragment at the
// rung its share allows, every block at the blocks' rung), without building it; false with the
// refusal `deposit_patch_fits` would make.
bool deposit_patch_triangles(const DepositPatch& patch, u64& out, std::string& error);
// A piece of a patch: an ordinary patch over a rectangle of the whole's samples, its corner
// `sample_x`, `sample_z` samples from the whole's.
struct DepositPiece {
  DepositPatch patch;
  u32 sample_x = 0;
  u32 sample_z = 0;
};
// G191: the pieces a patch is handed as, so each one's finest form holds at most `triangles_max`
// triangles. A patch under it is one piece, itself. Over it the patch is cut along a line of
// samples (its longer side's, where the deposit's triangles balance), and again, up to 16 pieces;
// each piece keeps the whole's samples, its blocks by their centres, the whole's rock and material,
// and a share of its fragments and budget by its deposit's cells, so the bed's steps and the
// stones' rungs are the whole's. The pieces' borders are locked as a patch's are, so they meet at
// every coarse form. A function of the patch alone.
void deposit_patch_pieces(const DepositPatch& whole, Vector<DepositPiece>& out,
                          u64 triangles_max = k_deposit_piece_triangles);
// How many times `solve_deposit` has built a mesh in this process: a statistic for tests.
u64 deposit_solve_count();
// G191: how many containers `build_deposit_container` built again at half their budget because
// the first build was over its cluster limit: a statistic for tests and the walk's log.
u64 deposit_builds_reduced();
// T120 (rocks.md, "A tile solves"): how many plans the deposits generator has made in this
// process. An entry opened twice on the same ground (the scene reader's, then the world's
// placements consumer's) plans once: the second open holds the first's plan by its inputs' key.
u64 deposit_plans_made();
u64 deposit_source_hash(const DepositPatch& patch);
// `clusters_max` (G191): over it the patch is built again at half its budget, fragments and block
// rung, up to eight times, and refused by name past that; the host's limit unless a test asks less.
bool build_deposit_container(const DepositPatch& patch, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                             const materials::Pack* pack = nullptr,
                             u32 clusters_max = k_deposit_clusters_max);
}  // namespace engine::rocks
