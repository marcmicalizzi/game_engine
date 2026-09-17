#pragma once

// glTF 2.0 import (docs/plan/07-content-pipeline.md section 7.2): the interchange format every
// DCC tool and generator writes, turned into the plain mesh the cluster builders take.
//
// One file becomes one `MeshData`. The default scene (or scene 0) is traversed recursively,
// every primitive of every node's mesh is appended to a single vertex and index space with the
// node's world transform baked into the positions (and the inverse transpose into the normals),
// and each primitive keeps a range into `indices` plus its material. That is exactly the shape
// `geometry::build_clusters` and `geometry::build_cluster_lod` want: positions, indices, and an
// `AttributeSource` over the normals and UVs, which `attribute_source` below hands over.
//
// What this module deliberately does not do: decode image pixels (the bytes or the URI are
// handed on for foundation/image and the texture pipeline), touch the GPU, evaluate animations
// or skins, read cameras and lights, or split the merged mesh back into parts. Draco and
// meshopt-compressed buffer views are not decoded either, so such files fail to load.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

// Metallic-roughness material parameters. Texture slots are indices into MeshData::images, or
// -1 when the material has no such texture. Factors default to the glTF defaults.
struct Material {
  std::string name;
  Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
  f32 metallic = 1.0f;
  f32 roughness = 1.0f;
  i32 base_color_image = -1;
  i32 normal_image = -1;
};

// An image referenced by a material, undecoded. `bytes` holds the encoded file for images that
// came from a GLB buffer view or a base64 data URI; `uri` names the file for external images,
// relative to the glTF file's directory. Decoding is foundation/image's job.
struct ImageRef {
  std::string name;
  std::string uri;
  std::string mime_type;  // "image/png", "image/jpeg"; empty when the file does not say
  Vector<u8> bytes;
};

// One glTF primitive as a range of MeshData::indices. `material` indexes MeshData::materials,
// or is -1 when the primitive has none.
struct Primitive {
  u32 first_index = 0;
  u32 index_count = 0;
  i32 material = -1;
};

// Every primitive of the scene merged into one vertex and index space, in world space.
// `normals` and `uvs` are either empty (no primitive had that attribute) or the same length as
// `positions`; a primitive that lacks an attribute the file has elsewhere contributes zeros.
struct MeshData {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  Vector<Primitive> primitives;
  Vector<Material> materials;
  Vector<ImageRef> images;
};

// Loads a .gltf (external or embedded buffers) or .glb file. External buffers and images are
// resolved relative to the file's directory through foundation/io. `out` is replaced, and left
// empty on failure. Returns false and fills `error` (when not null) on any failure: an
// unreadable file, invalid JSON or GLB framing, a primitive that is not triangles, a missing
// POSITION attribute, an attribute of the wrong shape, or an index out of range.
bool load_gltf(std::string_view path, MeshData& out, std::string* error = nullptr);

// The same from bytes already in memory (either form; the GLB magic selects). `base_dir` is the
// directory external buffer and image URIs are resolved against; pass an empty view to refuse
// external references.
bool load_gltf_memory(std::span<const u8> bytes, std::string_view base_dir, MeshData& out,
                      std::string* error = nullptr);

// Spans over the mesh's normals and UVs for geometry::build_clusters and
// geometry::build_cluster_lod. Absent attributes stay empty, which is what the builders expect:
// they compute smooth normals and leave UVs at zero.
geometry::AttributeSource attribute_source(const MeshData& mesh) noexcept;

}  // namespace engine::assets
