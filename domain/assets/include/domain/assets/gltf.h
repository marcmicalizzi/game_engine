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
// A **skinned** primitive is the one exception to the flattening, and it has to be: a skinned
// mesh's vertices are placed by its joints, so baking the mesh node's transform into them would
// apply the placement twice. Such a primitive keeps its positions and normals in **bind space**
// exactly as the accessors hold them, and the node transform that was not applied is recorded on
// the skin instead (`Skin::node_transform`). The per-vertex influences arrive as
// `geometry::SkinBinding`s in `MeshData::skin_bindings`, the joint hierarchy as `MeshData::nodes`,
// and the animation curves as `MeshData::animations`, all of which `domain/anim` turns into a
// `Skeleton`, a `Pose`, and a `Clip`.
//
// What this module deliberately does not do: decode image pixels (the bytes or the URI are
// handed on for foundation/image and the texture pipeline), touch the GPU, *evaluate* animations
// or skins (the curves are read, never sampled), read cameras and lights, or split the merged
// mesh back into parts. Draco and meshopt-compressed buffer views are not decoded either, so
// such files fail to load.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

// glTF `alphaMode`, as the one byte `Material::alpha_mode` and the cluster container store it.
inline constexpr u8 k_alpha_opaque = 0;  // alpha is ignored
inline constexpr u8 k_alpha_mask = 1;    // alpha < alpha_cutoff is not drawn
inline constexpr u8 k_alpha_blend = 2;   // alpha weighs the surface against what is behind it

// Metallic-roughness material parameters. Texture slots are indices into MeshData::images, or
// -1 when the material has no such texture; only TEXCOORD_0 is read, so a texture bound to any
// other UV set arrives as -1. Factors default to the glTF defaults.
struct Material {
  std::string name;
  Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
  Vec3 emissive{0.0f, 0.0f, 0.0f};
  f32 metallic = 1.0f;
  f32 roughness = 1.0f;
  f32 normal_scale = 1.0f;  // normalTexture.scale: how far the map tilts the normal
  f32 alpha_cutoff = 0.5f;  // alphaCutoff, meaningful for k_alpha_mask
  i32 base_color_image = -1;
  i32 metallic_roughness_image = -1;  // glTF packing: G roughness, B metallic
  i32 normal_image = -1;              // tangent space, UNORM, xyz remapped to -1..1
  i32 occlusion_image = -1;           // R ambient occlusion
  i32 emissive_image = -1;
  u8 alpha_mode = k_alpha_opaque;
  bool double_sided = false;
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
// or is -1 when the primitive has none; `skin` indexes MeshData::skins, or is -1 when the
// primitive's node had no skin — which is also what says whether its vertices were left in bind
// space or flattened to world space.
struct Primitive {
  u32 first_index = 0;
  u32 index_count = 0;
  i32 material = -1;
  i32 skin = -1;
};

// One node of the file's hierarchy, kept only when the file has skins or animations: a skeleton
// is a node subtree, and an animation channel names a node. `parent` is -1 for a root, and a
// node's parent always comes before it, so a single forward pass composes model transforms.
// `local` is the node's own TRS (the matrix form is decomposed), which is the bind pose for a
// joint, since glTF stores the bind pose as the nodes' own transforms.
struct Node {
  std::string name;
  Transform3 local;
  i32 parent = -1;
};

// A glTF skin: the joint palette a mesh's `SkinBinding` indices name, in the file's own order.
// `joints[i]` is an index into MeshData::nodes, and `inverse_bind[i]` takes a point from model
// space into joint i's space at bind, which is what turns a joint's animated model transform
// into a skinning matrix. glTF lets `inverseBindMatrices` be absent, in which case it is the
// identity for every joint; this fills the identity in rather than leaving the array short.
struct Skin {
  std::string name;
  Vector<i32> joints;         // into MeshData::nodes
  Vector<Mat4> inverse_bind;  // parallel to `joints`
  i32 skeleton_root = -1;     // the skin's `skeleton` node, or -1 when the file does not say
  // The world transform of the node that instanced this skin. glTF says a skinned mesh ignores
  // it — the joints place the vertices — so the importer does not bake it into the positions.
  // It is recorded because it is the only place the information survives, and a tool that wants
  // to plant a character where its author put it needs it.
  Mat4 node_transform = Mat4::identity();
};

// Animation sampler interpolation, as `AnimationSampler::interpolation` stores it.
inline constexpr u8 k_interp_linear = 0;  // lerp; slerp for a rotation
inline constexpr u8 k_interp_step = 1;    // hold the previous key
inline constexpr u8 k_interp_cubic = 2;   // CUBICSPLINE: in-tangent, value, out-tangent per key

// What an animation channel drives, as `AnimationChannel::path` stores it. glTF's `weights`
// path (morph targets) is skipped: this module reads no morph targets.
inline constexpr u8 k_path_translation = 0;
inline constexpr u8 k_path_rotation = 1;
inline constexpr u8 k_path_scale = 2;

// One keyframe curve. `times` is seconds, strictly increasing; `values` holds `components` floats
// per key for LINEAR and STEP (3 for a translation or a scale, 4 for a rotation quaternion) and
// three times that for CUBICSPLINE, which stores in-tangent, value, and out-tangent per key.
struct AnimationSampler {
  Vector<f32> times;
  Vector<f32> values;
  u8 interpolation = k_interp_linear;
  u8 components = 3;
};

struct AnimationChannel {
  i32 node = -1;  // into MeshData::nodes
  u32 sampler = 0;
  u8 path = k_path_translation;
};

struct Animation {
  std::string name;
  Vector<AnimationSampler> samplers;
  Vector<AnimationChannel> channels;
  f32 duration = 0.0f;  // the largest keyframe time of any of its samplers
};

// Every primitive of the scene merged into one vertex and index space: world space for a rigid
// primitive, **bind space** for a skinned one. `normals` and `uvs` are either empty (no primitive
// had that attribute) or the same length as `positions`; a primitive that lacks an attribute the
// file has elsewhere contributes zeros.
struct MeshData {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  // Per-vertex skin influences, empty unless the file has a skinned primitive, and otherwise the
  // same length as `positions`. A rigid primitive in a file that also has skinned ones contributes
  // the default binding (all weight on joint 0), which is what keeps the stream parallel.
  Vector<geometry::SkinBinding> skin_bindings;
  Vector<u32> indices;
  Vector<Primitive> primitives;
  Vector<Material> materials;
  Vector<ImageRef> images;
  // The node hierarchy, the skins, and the animation curves, all empty unless the file has a
  // skin or an animation: a static mesh imports exactly as it did before these existed, down to
  // the allocations. `skins` and `animations` index `nodes`.
  Vector<Node> nodes;
  Vector<Skin> skins;
  Vector<Animation> animations;
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

// The identity of a source mesh, for a content-addressed cache of what is built from it
// (docs/plan/07-content-pipeline.md §7.3): the file's own bytes hashed with `core/hash`, mixed
// with the bytes of every external buffer a `.gltf` names in `buffers[].uri`. A `.glb` carries
// its buffer inside, and a data URI is already part of the text, so neither adds anything. An
// external buffer that cannot be read contributes its length as zero rather than failing: the
// load that follows reports it properly, and the hash still changed with the glTF that names
// it. **External** images are deliberately not hashed: a container stores their paths and the
// renderer reads those files every time, so a repainted texture shows up without a rebuild. An
// image the file *embeds* needs no special case and gets none — it is part of the file's own
// bytes, so the hash already covers it, which is what keeps the cache key sound now that a
// container carries those bytes (`geometry::ClusterSection::Images`). Returns false and fills
// `error` when the mesh file itself cannot be read or parsed.
bool source_mesh_hash(std::string_view path, u64& out, std::string* error = nullptr);

// Spans over the mesh's normals, UVs, and skin bindings for geometry::build_clusters and
// geometry::build_cluster_lod. Absent attributes stay empty, which is what the builders expect:
// they compute smooth normals, leave UVs at zero, and build an unskinned mesh. The joint count
// is the widest skin of the file, which is the palette a merged mesh's indices live in.
geometry::AttributeSource attribute_source(const MeshData& mesh) noexcept;

}  // namespace engine::assets
