#pragma once

// A scene mesh a generator makes (scene_flora.cpp; docs/subsystems/renderer.md, "The scene file"):
// a path like `content/flora/date-palm.flora.json`, built through the derived-data cache by the
// mesh generator its name gives (`assets::MeshGeneratorRegistry`), the flora capability's first.

#include <domain/geometry/cluster_file.h>
#include <systems/renderer/scene.h>

#include <string>

namespace engine::content_build {
class ImageIdentities;
}

namespace engine::renderer {

// True when `path` names a mesh generator's file (`<name>.<generator>.json`), which only
// `load_generated_mesh` reads.
bool is_generated_mesh(const std::string& path);

// Builds or finds the container for a generated mesh: the cache entry its source hash and the
// scene's LOD options address when the scene reads the cache, built by the generator and written
// there on a miss; built in memory when it does not. `container` is where the bytes are on disk,
// empty for one built in memory; `cache` is "hit", "miss" or "none". False, with a sentence, for a
// generator this build does not carry (naming its capability), a file it refuses, or bytes that
// do not hash to `expected_hash` when that is not zero. `identities`, when not null, is the load's
// table of its source images (content_build/image_identities.h): a baked tree's source hash takes
// its base colours' hashes from it, so a cooked load reads none of them (roadmap W75).
bool load_generated_mesh(const std::string& path, const SceneDesc& desc, u64 expected_hash,
                         geometry::ClusterFileData& data, std::string& container,
                         const char*& cache, u64& source_hash, std::string& error,
                         content_build::ImageIdentities* identities = nullptr);

}  // namespace engine::renderer
