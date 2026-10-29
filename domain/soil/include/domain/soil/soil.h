#pragma once
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>

#include <schemas/soil.h>
#include <string>
#include <string_view>
namespace engine::soil {
inline constexpr u32 k_soil_version = 3;
inline constexpr const char* k_determinism = "derived";
struct Report {
  u32 triangles = 0, roots = 0, stones = 0, budget = 0;
  f32 cell_m = 0;
};
bool in_range(const SoilFile& file, std::string& error);
bool read_file(std::string_view text, SoilFile& out, std::string& error);
std::string canonical_json(const SoilFile& file);
u64 source_hash(const SoilFile& file);
u32 triangle_budget(const SoilFile& file) noexcept;
// Original ground sampled bilinearly. Public consumers validate the recipe first.
f32 ground_height(const GroundPatch& ground, f32 x, f32 z) noexcept;
// Height delta of the excavated lower surface; bank overhangs live in the companion mesh.
f32 terrain_delta(const SoilFile& file, f32 x, f32 z) noexcept;
// Sample through the public ground seam, retaining f64 world positions.
bool sample_ground(const scene_gen::Ground& ground, WorldPos origin, f32 radius_m, f32 spacing_m,
                   GroundPatch& out, std::string& error);
// Transactional in memory: validation failure leaves both arguments unchanged. No yaw/scale.
// Records original terrain in file.ground, then edits only the feature's footprint in terrain.
bool cook(SoilFile& file, tileset::Heightfield& terrain, WorldPos origin, std::string& error);
bool solve(const SoilFile& file, assets::MeshData& out, Report& report, std::string& error);
bool build_container(const SoilFile& file, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error);
}  // namespace engine::soil
