#pragma once

// A scene file's text read into the schema's `engine.scene.Scene`: the first stage of
// `read_scene_file` (scene.h), before anything of the file is expanded, opened or loaded. Apart so
// that what it costs — a forest staging's file is 172 MB — can be measured with no device and no
// generator (bench/scene_read_bench.cpp; roadmap D40).

#include <string>
#include <string_view>

namespace engine::scene {
struct Scene;
}

namespace engine::renderer {

// The text's grammar and the schema's, `engine.scene.Scene` with no unknown field, a water channel
// block written before W120 migrated from its `fit` (water_channel.h). `without_water_channels`
// leaves the file's channels unread (`SceneFileOptions::without_water_channels`). False with
// `error` saying where: `<path>:<line>: <message>` for the text, `<path>: <diagnostics>` for the
// schema.
bool read_scene_text(std::string_view text, const std::string& path, bool without_water_channels,
                     scene::Scene& out, std::string& error);

}  // namespace engine::renderer
