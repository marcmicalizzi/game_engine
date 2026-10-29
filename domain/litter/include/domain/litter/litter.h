#pragma once
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/deadwood/deadwood.h>

#include <schemas/litter.h>
namespace engine::litter {
inline constexpr u32 k_litter_version = 1;
inline constexpr const char* k_determinism = "derived";
struct Profile {
  f32 length_m, width_m;
  u32 count, budget;
};
struct Report {
  u32 particles = 0, triangles = 0, budget = 0, segments = 0;
};
Profile profile(Kind kind);
bool in_range(const Recipe& p, std::string& error);
bool read_file(std::string_view text, Recipe& out, std::string& error);
std::string canonical_json(const Recipe& p);
u64 source_hash(const Recipe& p);
// Exact recipe used by solve, also usable as a standalone deadwood asset.
deadwood::DeadwoodFile wood_recipe(const Recipe& p);
bool solve(const Recipe& p, assets::MeshData& out, Report& report, std::string& error);
bool build_container(const Recipe& p, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error);
}  // namespace engine::litter
