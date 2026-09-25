#pragma once

// The synthetic kits' GLB writer (src/synthetic_kit.cpp), shared by the kit of boxes and the
// synthetic block kit (src/synthetic_blocks.cpp): one primitive of positions, normals and u32
// indices with one metallic-roughness material, and the files of a kit written into a directory.
// Private to the module.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <string>

namespace engine::ruins {

Vector<u8> mesh_glb(const Vector<Vec3>& positions, const Vector<Vec3>& normals,
                    const Vector<u32>& indices, const Vec3& colour, const char* generator);

// Creates `dir` and writes `glbs[i]` to `dir/names[i]`.
bool write_kit_files(const std::string& dir, const Vector<std::string>& names,
                     const Vector<Vector<u8>>& glbs, std::string* error);

}  // namespace engine::ruins
