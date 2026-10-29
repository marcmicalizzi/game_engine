#pragma once
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/motion.h>
#include <domain/geometry/flora_species.h>

#include <schemas/wetland.h>

namespace engine::wetland {
inline constexpr u32 k_wetland_version = 4;
// Dimensions and provenance: docs/experiments/wetland-plants.md.
// Names, budgets and runtime motion traits live only in geometry's species catalogue.
struct Profile {
  geometry::FloraSpecies species;
  f32 height_m, leaf_length_m, leaf_width_m, stem_radius_m;
  u32 shoots;
  f32 crown_radius_m;
};
struct Report {
  u32 triangles = 0, live_shoots = 0, dead_shoots = 0, parts = 0, detail = 0, budget = 0;
};
std::span<const Profile> profiles() noexcept;
const Profile* profile(std::string_view name) noexcept;
bool in_range(const Plant& recipe, std::string& error);
bool read_file(std::string_view text, Plant& out, std::string& error);
std::string canonical_json(const Plant& recipe);
u64 source_hash(const Plant& recipe);
bool solve(const Plant& recipe, assets::MeshData& mesh, flora::Motion& motion, Report& report,
           std::string& error);
bool build_container(const Plant& recipe, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error);
}  // namespace engine::wetland
