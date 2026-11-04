#pragma once

// Static, terrain-aware rockfall and sediment authoring (G124; docs/subsystems/rocks.md).
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>

namespace engine::rocks {
inline constexpr u32 k_deposit_version = 1;

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
};

// Pure finite-window solve; no I/O, persistent state or dependence on tile arrival order.
// Refuses absent/nonfinite ground, incompatible grids and exceeded budgets. Output clears on error.
bool plan_deposits(const DepositLayer& layer, const scene_gen::Ground& ground, DepositPlan& out,
                   std::string& error);
bool deposit_patch_in_range(const DepositPatch& patch, std::string& error);
bool solve_deposit(const DepositPatch& patch, assets::MeshData& out, std::string& error);
u64 deposit_source_hash(const DepositPatch& patch);
bool build_deposit_container(const DepositPatch& patch, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                             const materials::Pack* pack = nullptr);
}  // namespace engine::rocks
