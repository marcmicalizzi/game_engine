#pragma once

#include <core/containers/vector.h>
#include <domain/assets/gltf.h>
#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/terrain.h>

namespace engine::renderer {
// Cold preparation; decoded sources enter the existing mipmapped texture build. Heights never
// depend on these layers. Paths must already be resolved against the scene directory.
// `masked`: the terrain names `masks`, so the layers are a palette and the budget is per tile.
bool validate_terrain_materials(const scene::TerrainMaterials& materials, std::string* error,
                                bool masked = false);
bool append_terrain_materials(const TerrainDesc& terrain, assets::MeshData& mesh,
                              std::string* error);
u64 terrain_materials_hash(const scene::TerrainMaterials& materials);

// **The mask level as the GPU reads it** (renderer.md, "Ground materials by place"): the terrain's
// ground provider asked for its masks (`scene_gen::GroundOps::masks`, built on a miss), and every
// tile it holds laid out after a page table over the level's tiles — a slot per resident tile,
// `gfx::k_terrain_no_mask_tile` for none — with the record's mask fields (`mask` left for the
// caller, who knows the buffer's address). Empty words for a provider with no masks. False, with a
// sentence, for masks that were named and not built, a tile that would not read, or a level larger
// than the block's `resident_mib`.
struct TerrainMaskUpload {
  Vector<u32> words;
  gfx::TerrainRecord record;
  u32 tiles = 0;
};
bool prepare_terrain_masks(const TerrainDesc& terrain, TerrainMaskUpload& out, std::string* error);
}  // namespace engine::renderer
