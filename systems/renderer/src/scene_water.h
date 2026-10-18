#pragma once

// The scene file's `water` block (engine.scene.WaterBody; docs/subsystems/water.md), read by
// `read_scene_file` in one call after its placement entries and before its terrain.

#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <schemas/scene.h>
#include <string>

namespace engine::renderer {

// The block's bodies through the water capability's placement generator ("water", found in the
// scene-generator registry like any generator): one mesh and one instance per body, appended after
// everything `out` has so far, each mesh's `SceneMeshInfo` carrying the body and its resolved
// shading (`SceneWater`) and the origin "water". Nothing for a file with no block — the scene is
// then the one it always was. False with a sentence for a build that carries no water capability
// (ENGINE_WITH_WATER off, the minimal build), or a body the capability refuses.
bool read_scene_water(const scene::Scene& file, const std::string& path, const std::string& dir,
                      const TerrainSampler& ground, SceneDesc& out, std::string& error);

}  // namespace engine::renderer
