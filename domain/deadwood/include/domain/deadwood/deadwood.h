#pragma once

#include <domain/assets/mesh_generator.h>
#include <domain/flora/flora.h>

#include <schemas/deadwood.h>

namespace engine::deadwood {
inline constexpr u32 k_deadwood_version = 1;

// Cold cook diagnostics. No solver state or allocations in the frame loop.
struct Report {
  u32 source_triangles = 0, triangles = 0, wood_triangles = 0, foliage_triangles = 0;
  u32 bent_parts = 0, broken_parts = 0, retained_parts = 0, budget = 0;
  f32 lift_m = 0, minimum_clearance_m = 0;
};
bool read_file(std::string_view text, DeadwoodFile& out, std::string& error);
bool in_range(const DeadwoodFile& file, std::string& error);
std::string canonical_json(const DeadwoodFile& file);
u64 source_hash(const DeadwoodFile& file);
// Pose is baked in. Place at the site's origin without reapplying pitch/roll.
// Failure clears output; the source tree and its cached geometry are never changed.
bool solve(const DeadwoodFile& file, assets::MeshData& mesh, flora::Motion& motion, Report& report,
           std::string& error);
bool build_container(const DeadwoodFile& file, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error);
}  // namespace engine::deadwood
