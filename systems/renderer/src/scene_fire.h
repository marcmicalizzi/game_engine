#pragma once

// The scene file's `fires` and `lamps` (engine.scene.Fire and Lamp; fire.h, renderer.md "Fires"
// and "Lamps"), read by `read_scene_file` in one call beside its medium.

#include <systems/renderer/scene.h>

#include <schemas/scene.h>
#include <string>

namespace engine::renderer {

// Every fire of the file into `out.fires`, checked by `read_fire`; nothing for a file with none.
// False with a sentence naming the fire and the field.
bool read_scene_fires(const scene::Scene& file, const std::string& path, SceneDesc& out,
                      std::string& error);
// Every lamp of the file into `out.lamps`, checked by `read_lamp`: false with a sentence naming the
// lamp and the field, or the count when the file names more than `k_max_lamps` (R86).
bool read_scene_lamps(const scene::Scene& file, const std::string& path, SceneDesc& out,
                      std::string& error);

}  // namespace engine::renderer
