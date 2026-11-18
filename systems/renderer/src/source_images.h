#pragma once

// The load's embedded image bytes, let go once the built textures stand in for them (W51-3;
// renderer.md, "The embedded image bytes go once the textures are built"). `source_image_bytes`,
// the read-back, is public in scene.h beside `SourceMesh`.

#include <core/base/types.h>
#include <core/containers/vector.h>

namespace engine::renderer {

struct SourceMesh;

struct ImageBytesReleased {
  u32 meshes = 0;
  u32 images = 0;
  u64 bytes = 0;
};

// Empties every `data.images[i].bytes` of each mesh whose sampled images all have their built
// `.tex` under its root and whose container holds the bytes, and marks it
// (`SourceMesh::image_bytes_released`). A mesh with no derived-data root, no container, or an
// image not built keeps every byte. Called by `load_scene` once its texture pass has built what it
// builds and before the forms copy the meshes, so no copy carries them.
ImageBytesReleased release_built_image_bytes(Vector<SourceMesh>& sources);

}  // namespace engine::renderer
