#pragma once

// Static, terrain-aware rockfall and sediment authoring (G128; docs/subsystems/rocks.md), with the
// cones under gullies, the blocks shed below the faces and the formation's lithology (G143).
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>

namespace engine::rocks {
// 2 since G143 (2026-11-10): a bed's bare rim on steep ground sinks below the face it meets.
inline constexpr u32 k_deposit_version = 2;

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
u64 deposit_source_hash(const DepositPatch& patch);
bool build_deposit_container(const DepositPatch& patch, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                             const materials::Pack* pack = nullptr);
}  // namespace engine::rocks
