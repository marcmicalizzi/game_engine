#pragma once

// Static, terrain-aware rockfall and sediment authoring (G128; docs/subsystems/rocks.md), with the
// cones under gullies, the blocks shed below the faces and the formation's lithology (G143).
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>

namespace engine::rocks {
// 2 since G143 (2026-11-10): a bed's bare rim on steep ground sinks below the face it meets.
// 3 (G143, second agent): the bed's outline, its sink and its normals come from the plan.
// 4 (G143's third pass): the bed's top is rubble, its clasts anchored to the world, the loose
// fragments of the largest band alone, the toe's clasts spread over metres.
inline constexpr u32 k_deposit_version = 4;
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
};

// Pure finite-window solve; no I/O, persistent state or dependence on tile arrival order.
// Refuses absent/nonfinite ground, incompatible grids and exceeded budgets. Output clears on error.
bool plan_deposits(const DepositLayer& layer, const scene_gen::Ground& ground, DepositPlan& out,
                   std::string& error);
// The plan's ground converges here (G143): the contour curvature over `scale_samples` samples,
// per metre, positive in a hollow; zero on a face whose contours are straight.
f64 deposit_convergence(const DepositPlan& plan, u32 i, u32 scale_samples);
bool deposit_patch_in_range(const DepositPatch& patch, std::string& error);
bool solve_deposit(const DepositPatch& patch, assets::MeshData& out, std::string& error);
// Every refusal `solve_deposit` would make, without building the mesh (G143; rocks.md, "A tile
// solves"): what the generator's `open` checks each patch with, so it solves none.
bool deposit_patch_fits(const DepositPatch& patch, std::string& error);
// How many times `solve_deposit` has built a mesh in this process: a statistic for tests.
u64 deposit_solve_count();
u64 deposit_source_hash(const DepositPatch& patch);
bool build_deposit_container(const DepositPatch& patch, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                             const materials::Pack* pack = nullptr);
}  // namespace engine::rocks
