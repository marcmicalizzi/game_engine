#pragma once

#include <domain/assets/gltf.h>
#include <systems/renderer/terrain.h>

namespace engine::renderer {
// Cold preparation; decoded sources enter the existing mipmapped texture build. Heights never
// depend on these layers. Paths must already be resolved against the scene directory.
bool validate_terrain_materials(const scene::TerrainMaterials& materials, std::string* error);
bool append_terrain_materials(const TerrainDesc& terrain, assets::MeshData& mesh,
                              std::string* error);
u64 terrain_materials_hash(const scene::TerrainMaterials& materials);
}  // namespace engine::renderer
