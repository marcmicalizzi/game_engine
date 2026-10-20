#pragma once

// The scene file's `fires` (engine.scene.Fire; fire.h, renderer.md "Fires"), read by
// `read_scene_file` in one call beside its medium.

#include <systems/renderer/scene.h>

#include <schemas/scene.h>
#include <string>

namespace engine::renderer {

// Every fire of the file into `out.fires`, checked by `read_fire`; nothing for a file with none.
// False with a sentence naming the fire and the field.
bool read_scene_fires(const scene::Scene& file, const std::string& path, SceneDesc& out,
                      std::string& error);

}  // namespace engine::renderer
