#include <core/hash/hash.h>
#include <domain/assets/gltf.h>
#include <foundation/io/vfs.h>

// cgltf is a single-header glTF 2.0 parser (MIT, third_party/LICENSES.md); the implementation
// is compiled into this one translation unit.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace engine::assets {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

const char* result_text(cgltf_result result) noexcept {
  switch (result) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
    default: return "unknown error";
  }
}

const char* topology_text(cgltf_primitive_type type) noexcept {
  switch (type) {
    case cgltf_primitive_type_points: return "points";
    case cgltf_primitive_type_lines: return "lines";
    case cgltf_primitive_type_line_loop: return "line loop";
    case cgltf_primitive_type_line_strip: return "line strip";
    case cgltf_primitive_type_triangles: return "triangles";
    case cgltf_primitive_type_triangle_strip: return "triangle strip";
    case cgltf_primitive_type_triangle_fan: return "triangle fan";
    default: return "unknown";
  }
}

// --- file access through foundation/io --------------------------------------------------------
// cgltf's default reader is fopen() on a narrow path, which mangles non-ASCII directories on
// Windows. These callbacks route external buffers and .bin files through io::read_file instead,
// so the module has exactly one way to touch the disk.

void* alloc_bytes(const cgltf_memory_options* memory, cgltf_size size) {
  if (memory != nullptr && memory->alloc_func != nullptr)
    return memory->alloc_func(memory->user_data, size);
  return std::malloc(size);
}

void free_bytes(const cgltf_memory_options* memory, void* bytes) {
  if (memory != nullptr && memory->free_func != nullptr) {
    memory->free_func(memory->user_data, bytes);
    return;
  }
  std::free(bytes);
}

cgltf_result read_through_io(const cgltf_memory_options* memory, const cgltf_file_options*,
                             const char* path, cgltf_size* size, void** data) {
  std::string contents;
  const io::Status status = io::read_file(path, contents);
  if (status == io::Status::NotFound) return cgltf_result_file_not_found;
  if (status != io::Status::Ok) return cgltf_result_io_error;

  const cgltf_size wanted = (size != nullptr && *size != 0) ? *size : contents.size();
  if (wanted > contents.size()) return cgltf_result_data_too_short;
  void* buffer = alloc_bytes(memory, wanted != 0 ? wanted : 1);
  if (buffer == nullptr) return cgltf_result_out_of_memory;
  std::memcpy(buffer, contents.data(), wanted);
  if (size != nullptr) *size = wanted;
  *data = buffer;
  return cgltf_result_success;
}

void release_through_io(const cgltf_memory_options* memory, const cgltf_file_options*, void* data) {
  free_bytes(memory, data);
}

// Owns the parsed document so every error path frees it.
struct Document {
  cgltf_data* data = nullptr;
  Document() = default;
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;
  ~Document() {
    if (data != nullptr) cgltf_free(data);
  }
};

// --- small helpers ----------------------------------------------------------------------------

std::string decoded_uri(const char* uri) {
  std::string text(uri);
  const cgltf_size length = cgltf_decode_uri(text.data());
  text.resize(length);
  return text;
}

// "data:image/png;base64,iVBOR..." -> the media type and the decoded bytes. Only base64 data
// URIs exist in practice, and they are the only ones glTF allows for buffers.
bool decode_data_uri(const char* uri, std::string& mime_type, Vector<u8>& out) {
  const char* comma = std::strchr(uri, ',');
  if (comma == nullptr) return false;
  const std::string_view header(uri + 5, static_cast<usize>(comma - uri) - 5);  // past "data:"
  const std::string_view marker(";base64");
  if (header.size() < marker.size() || header.substr(header.size() - marker.size()) != marker)
    return false;
  mime_type = std::string(header.substr(0, header.size() - marker.size()));

  const char* encoded = comma + 1;
  usize characters = std::strlen(encoded);
  while (characters > 0 && encoded[characters - 1] == '=')
    --characters;
  const usize remainder = characters % 4;
  if (remainder == 1) return false;
  const usize size = characters / 4 * 3 + (remainder == 2 ? 1 : (remainder == 3 ? 2 : 0));
  if (size > std::numeric_limits<u32>::max()) return false;

  out.clear();
  if (size == 0) return true;
  cgltf_options options{};
  void* bytes = nullptr;
  if (cgltf_load_buffer_base64(&options, size, encoded, &bytes) != cgltf_result_success)
    return false;
  out.resize(static_cast<u32>(size));
  std::memcpy(out.data(), bytes, size);
  free_bytes(nullptr, bytes);
  return true;
}

Mat4 local_transform(const cgltf_node& node) noexcept {
  cgltf_float m[16];
  cgltf_node_transform_local(&node, m);  // column-major, like core/math
  return Mat4(Vec4(m[0], m[1], m[2], m[3]), Vec4(m[4], m[5], m[6], m[7]),
              Vec4(m[8], m[9], m[10], m[11]), Vec4(m[12], m[13], m[14], m[15]));
}

// A node's own transform as a TRS. glTF stores a node either as translation/rotation/scale or as
// one matrix, and a joint's node transform *is* its bind pose, so a skeleton wants the TRS form:
// blending, retargeting, and the standard skeleton's offsets are all rotations and translations,
// and recovering them from a matrix once at import is cheaper and more accurate than doing it
// per pose. glTF requires a node matrix to be decomposable (no shear), so the column lengths are
// the scales and the normalized columns are the rotation.
Transform3 node_trs(const cgltf_node& node) noexcept {
  Transform3 out;
  if (node.has_matrix == 0) {
    out.position = Vec3{node.translation[0], node.translation[1], node.translation[2]};
    out.rotation =
        normalize(Quat{node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]});
    out.scale = Vec3{node.scale[0], node.scale[1], node.scale[2]};
    return out;
  }
  const cgltf_float* m = node.matrix;
  const Vec3 c0{m[0], m[1], m[2]};
  const Vec3 c1{m[4], m[5], m[6]};
  const Vec3 c2{m[8], m[9], m[10]};
  out.position = Vec3{m[12], m[13], m[14]};
  Vec3 scale{length(c0), length(c1), length(c2)};
  // A negative determinant is a mirror; the flip goes on x, which is the usual convention and
  // keeps the rotation a rotation rather than a reflection the quaternion cannot express.
  if (dot(cross(c0, c1), c2) < 0.0f) scale.x = -scale.x;
  const Vec3 axis_x = std::fabs(scale.x) > 1.0e-8f ? c0 * (1.0f / scale.x) : Vec3{1.0f, 0.0f, 0.0f};
  const Vec3 axis_y = scale.y > 1.0e-8f ? c1 * (1.0f / scale.y) : Vec3{0.0f, 1.0f, 0.0f};
  const Vec3 axis_z = scale.z > 1.0e-8f ? c2 * (1.0f / scale.z) : Vec3{0.0f, 0.0f, 1.0f};
  out.rotation = quat_from_mat3(Mat3(axis_x, axis_y, axis_z));
  out.scale = scale;
  return out;
}

// Unpacks a whole accessor into `out`, applying sparse data and component conversion.
bool unpack_accessor(const cgltf_accessor& accessor, u32 components, const char* what,
                     Vector<f32>& out, std::string* error) {
  if (cgltf_num_components(accessor.type) != components)
    return fail(error, std::string("glTF: ") + what + " is not a vector of " +
                           std::to_string(components) + " components");
  const cgltf_size total = accessor.count * components;
  if (total > std::numeric_limits<u32>::max())
    return fail(error, std::string("glTF: ") + what + " has too many components");
  out.resize(static_cast<u32>(total));
  if (total == 0) return true;
  if (cgltf_accessor_unpack_floats(&accessor, out.data(), total) != total)
    return fail(error, std::string("glTF: could not read ") + what +
                           " (compressed or unsupported buffer view?)");
  return true;
}

i32 image_slot(const cgltf_data& data, const cgltf_texture_view& view) noexcept {
  if (view.texture == nullptr) return -1;
  if (view.texcoord != 0) return -1;  // TEXCOORD_0 only; the mesh carries one UV set
  const cgltf_image* image = view.texture->image;
  if (image == nullptr && view.texture->has_basisu != 0) image = view.texture->basisu_image;
  if (image == nullptr && view.texture->has_webp != 0) image = view.texture->webp_image;
  if (image == nullptr) return -1;
  return static_cast<i32>(cgltf_image_index(&data, image));
}

// --- the merge ---------------------------------------------------------------------------------

struct Context {
  const cgltf_data* gltf = nullptr;
  MeshData* mesh = nullptr;
  std::string* error = nullptr;
  bool saw_normals = false;
  bool saw_uvs = false;
  bool saw_skin = false;
  bool has_skins = false;   // the file declares skins, so the binding stream is kept parallel
  Vector<u32> node_remap;   // cgltf node index -> MeshData::nodes index
  Vector<u8> skin_placed;   // whether a skin's node_transform has been recorded
  Vector<f32> floats;       // scratch for one attribute accessor
  Vector<f32> more_floats;  // scratch for a second accessor read alongside the first
  Vector<u32> widened;      // scratch for one index accessor
  // Morph targets. `morph_of_mesh[m]` is where glTF mesh m's channels start in `MeshData::morph`,
  // or ~0u when that mesh has none or has not been reached yet: within a mesh, target k of every
  // primitive is one channel, and two different meshes' targets are unrelated even where they
  // share a name, so the flatten cannot merge them.
  Vector<u32> morph_of_mesh;
  Vector<Vec3> local_normals;   // this primitive's NORMAL before the node transform
  Vector<Vec3> target_deltas;   // scratch for one target's POSITION deltas
  Vector<Vec3> target_normals;  // scratch for one target's NORMAL deltas
};

bool read_attribute(Context& ctx, const cgltf_accessor& accessor, u32 components,
                    const char* what) {
  return unpack_accessor(accessor, components, what, ctx.floats, ctx.error);
}

// JOINTS_0 and WEIGHTS_0 of one primitive into `MeshData::skin_bindings`, which the caller has
// already grown to cover this primitive's vertices. Joint indices come through
// `cgltf_accessor_read_uint`, which widens the u8 and u16 forms both exporters use; weights come
// through the float unpacker, which handles the float and the normalized-integer forms; and
// `geometry::make_skin_binding` renormalizes the four of them to sum to 255.
bool append_skin_bindings(Context& ctx, const cgltf_primitive& primitive, u32 base, u32 vertices,
                          i32 skin_index) {
  MeshData& out = *ctx.mesh;
  const cgltf_accessor* joints = cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0);
  if (joints == nullptr) return true;  // a skinned node whose primitive carries no influences
  const cgltf_accessor* weights = cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0);
  if (joints->count != vertices)
    return fail(ctx.error, "glTF: JOINTS_0 and POSITION have different counts");
  if (cgltf_num_components(joints->type) != 4)
    return fail(ctx.error, "glTF: JOINTS_0 is not a vector of four components");
  if (weights != nullptr) {
    if (weights->count != vertices)
      return fail(ctx.error, "glTF: WEIGHTS_0 and POSITION have different counts");
    if (!unpack_accessor(*weights, 4, "WEIGHTS_0", ctx.more_floats, ctx.error)) return false;
  }
  const u32 palette = out.skins[static_cast<u32>(skin_index)].joints.size();
  for (u32 i = 0; i < vertices; ++i) {
    cgltf_uint quad[4] = {0, 0, 0, 0};
    if (cgltf_accessor_read_uint(joints, i, quad, 4) == 0)
      return fail(ctx.error, "glTF: could not read JOINTS_0 (a sparse or compressed accessor?)");
    u32 indices[4];
    for (u32 k = 0; k < 4; ++k) {
      if (quad[k] >= palette) {
        return fail(ctx.error, "glTF: JOINTS_0 names joint " + std::to_string(quad[k]) +
                                   ", outside the skin's " + std::to_string(palette) + " joints");
      }
      indices[k] = quad[k];
    }
    f32 influence[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    if (weights != nullptr) {
      for (u32 k = 0; k < 4; ++k)
        influence[k] = ctx.more_floats[i * 4 + k];
    }
    out.skin_bindings[base + i] = geometry::make_skin_binding(indices, influence);
  }
  ctx.saw_skin = true;
  return true;
}

// The morph targets of one primitive into the channels its **mesh** owns, which the caller has
// already created. A target's POSITION deltas are directions, so a rigid primitive's node
// transform reaches them through its linear part alone; a skinned primitive's vertices were left
// in bind space, and so are its deltas. A NORMAL delta is a *difference of two unit vectors*, so
// it cannot be transformed on its own: it is recomputed as the world normal of the displaced
// surface minus the world rest normal, which is why the primitive's own untransformed normals are
// kept beside it.
//
// A delta is kept only when it survives the stream's own 16-bit quantization — at least half a
// step in some component — so the sparsity is lossless with respect to what would be stored
// anyway rather than a second, invisible tolerance. The step is the channel's own peak over
// 32767, and both halves of that are computed here, over this primitive's share of the channel.
bool append_morph_targets(Context& ctx, const cgltf_primitive& primitive, u32 base, u32 vertices,
                          const Mat4& world, const Mat4& normal_transform, u32 channel_base) {
  MeshData& out = *ctx.mesh;
  const bool have_normals = ctx.local_normals.size() == vertices;
  for (cgltf_size t = 0; t < primitive.targets_count; ++t) {
    const cgltf_morph_target& target = primitive.targets[t];
    const cgltf_accessor* positions = nullptr;
    const cgltf_accessor* normals = nullptr;
    for (cgltf_size a = 0; a < target.attributes_count; ++a) {
      const cgltf_attribute& attribute = target.attributes[a];
      if (attribute.type == cgltf_attribute_type_position) positions = attribute.data;
      if (attribute.type == cgltf_attribute_type_normal) normals = attribute.data;
    }
    if (positions == nullptr) continue;  // a target that moves only tangents moves nothing here
    if (positions->count != vertices)
      return fail(ctx.error, "glTF: a morph target's POSITION count differs from the primitive's");
    if (!read_attribute(ctx, *positions, 3, "a morph target's POSITION")) return false;
    ctx.target_deltas.clear();
    ctx.target_deltas.reserve(vertices);
    for (u32 i = 0; i < vertices; ++i) {
      ctx.target_deltas.push_back(transform_direction(
          world, Vec3{ctx.floats[i * 3], ctx.floats[i * 3 + 1], ctx.floats[i * 3 + 2]}));
    }
    ctx.target_normals.clear();
    if (normals != nullptr && have_normals) {
      if (normals->count != vertices)
        return fail(ctx.error, "glTF: a morph target's NORMAL count differs from the primitive's");
      if (!read_attribute(ctx, *normals, 3, "a morph target's NORMAL")) return false;
      ctx.target_normals.reserve(vertices);
      for (u32 i = 0; i < vertices; ++i) {
        const Vec3 rest = ctx.local_normals[i];
        const Vec3 moved =
            rest + Vec3{ctx.floats[i * 3], ctx.floats[i * 3 + 1], ctx.floats[i * 3 + 2]};
        const Vec3 world_rest = transform_direction(normal_transform, rest);
        const Vec3 world_moved = transform_direction(normal_transform, moved);
        const Vec3 a = length_squared(world_rest) > 1.0e-20f ? normalize(world_rest) : Vec3{};
        const Vec3 b = length_squared(world_moved) > 1.0e-20f ? normalize(world_moved) : a;
        ctx.target_normals.push_back(b - a);
      }
    }

    f32 position_peak = 0.0f;
    f32 normal_peak = 0.0f;
    for (u32 i = 0; i < vertices; ++i) {
      const Vec3& d = ctx.target_deltas[i];
      position_peak = std::max(position_peak,
                               std::max(std::fabs(d.x), std::max(std::fabs(d.y), std::fabs(d.z))));
      if (i < ctx.target_normals.size()) {
        const Vec3& n = ctx.target_normals[i];
        normal_peak = std::max(normal_peak,
                               std::max(std::fabs(n.x), std::max(std::fabs(n.y), std::fabs(n.z))));
      }
    }
    const f32 position_floor = position_peak > 0.0f ? position_peak / 65534.0f : 0.0f;
    const f32 normal_floor = normal_peak > 0.0f ? normal_peak / 65534.0f : 0.0f;

    geometry::MorphChannelSource& channel = out.morph[channel_base + static_cast<u32>(t)];
    // A primitive's normal deltas join the channel only if every entry the channel already holds
    // has one: a channel is **one parallel array or none**, across all the primitives of its mesh.
    // Checking this primitive alone was not enough — a mesh whose first primitive's target has no
    // NORMAL and whose second one's does left the array shorter than `vertices` (the drop below
    // only ever ran for the primitive that lacked them, so it covered the other order).
    const bool channel_normals =
        !ctx.target_normals.empty() && channel.normal_deltas.size() == channel.vertices.size();
    for (u32 i = 0; i < vertices; ++i) {
      const Vec3& d = ctx.target_deltas[i];
      const bool moves_position = std::fabs(d.x) > position_floor ||
                                  std::fabs(d.y) > position_floor ||
                                  std::fabs(d.z) > position_floor;
      Vec3 n{};
      bool moves_normal = false;
      if (i < ctx.target_normals.size()) {
        n = ctx.target_normals[i];
        moves_normal = std::fabs(n.x) > normal_floor || std::fabs(n.y) > normal_floor ||
                       std::fabs(n.z) > normal_floor;
      }
      if (!moves_position && !moves_normal) continue;
      channel.vertices.push_back(base + i);
      channel.position_deltas.push_back(d);
      if (channel_normals) channel.normal_deltas.push_back(n);
    }
    // A channel whose normal deltas are present for one primitive and absent for another would
    // not be parallel to its vertices; the parallel form is the contract, so the array is dropped
    // outright when any primitive of the mesh did not supply one.
    if (!channel_normals) channel.normal_deltas.clear();
  }
  return true;
}

bool append_primitive(Context& ctx, const cgltf_primitive& primitive, const Mat4& world,
                      const Mat4& normal_transform, i32 skin_index, u32 morph_base) {
  MeshData& out = *ctx.mesh;
  if (primitive.type != cgltf_primitive_type_triangles)
    return fail(ctx.error, std::string("glTF: primitive topology is ") +
                               topology_text(primitive.type) + "; only triangles are accepted");

  const cgltf_accessor* positions =
      cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
  if (positions == nullptr) return fail(ctx.error, "glTF: a primitive has no POSITION attribute");
  const cgltf_size count = positions->count;
  if (count == 0) return fail(ctx.error, "glTF: a primitive has no vertices");

  const u32 base = out.positions.size();
  if (count > std::numeric_limits<u32>::max() - base)
    return fail(ctx.error, "glTF: the scene has more vertices than the mesh format holds");
  const u32 vertices = static_cast<u32>(count);

  if (!read_attribute(ctx, *positions, 3, "POSITION")) return false;
  out.positions.reserve(base + vertices);
  for (u32 i = 0; i < vertices; ++i) {
    const Vec3 p{ctx.floats[i * 3], ctx.floats[i * 3 + 1], ctx.floats[i * 3 + 2]};
    out.positions.push_back(transform_point(world, p));
  }
  // Normals and UVs stay parallel to the positions even for primitives that have none, and so do
  // the skin bindings — but only in a file that has skins at all, so a static mesh does not pay
  // eight bytes a vertex for a stream that is about to be thrown away.
  out.normals.resize(base + vertices);
  out.uvs.resize(base + vertices);
  if (ctx.has_skins) out.skin_bindings.resize(base + vertices);

  const cgltf_accessor* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
  ctx.local_normals.clear();
  if (normals != nullptr) {
    if (normals->count != count)
      return fail(ctx.error, "glTF: NORMAL and POSITION have different counts");
    if (!read_attribute(ctx, *normals, 3, "NORMAL")) return false;
    ctx.saw_normals = true;
    ctx.local_normals.reserve(vertices);
    for (u32 i = 0; i < vertices; ++i) {
      const Vec3 n{ctx.floats[i * 3], ctx.floats[i * 3 + 1], ctx.floats[i * 3 + 2]};
      ctx.local_normals.push_back(n);
      out.normals[base + i] = normalize(transform_direction(normal_transform, n));
    }
  }

  const cgltf_accessor* uvs = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
  if (uvs != nullptr) {
    if (uvs->count != count)
      return fail(ctx.error, "glTF: TEXCOORD_0 and POSITION have different counts");
    if (!read_attribute(ctx, *uvs, 2, "TEXCOORD_0")) return false;
    ctx.saw_uvs = true;
    for (u32 i = 0; i < vertices; ++i)
      out.uvs[base + i] = Vec2{ctx.floats[i * 2], ctx.floats[i * 2 + 1]};
  }

  // Influences are read only for a primitive whose node actually has a skin: JOINTS_0 on a node
  // with no skin names nothing, and those vertices were flattened to world space anyway.
  if (skin_index >= 0 && !append_skin_bindings(ctx, primitive, base, vertices, skin_index))
    return false;

  if (primitive.targets_count != 0 && morph_base != ~0u &&
      !append_morph_targets(ctx, primitive, base, vertices, world, normal_transform, morph_base)) {
    return false;
  }

  const u32 first = out.indices.size();
  if (primitive.indices != nullptr) {
    const cgltf_size index_count = primitive.indices->count;
    if (index_count == 0 || index_count % 3 != 0)
      return fail(ctx.error,
                  "glTF: the index count of a primitive is not a positive multiple of "
                  "three");
    if (index_count > std::numeric_limits<u32>::max() - first)
      return fail(ctx.error, "glTF: the scene has more indices than the mesh format holds");
    ctx.widened.resize(static_cast<u32>(index_count));
    if (cgltf_accessor_unpack_indices(primitive.indices, ctx.widened.data(), sizeof(u32),
                                      index_count) != index_count)
      return fail(ctx.error,
                  "glTF: could not read the indices of a primitive (sparse or "
                  "compressed accessors are not supported)");
    out.indices.reserve(first + static_cast<u32>(index_count));
    for (u32 i = 0; i < static_cast<u32>(index_count); ++i) {
      const u32 index = ctx.widened[i];
      if (index >= vertices)
        return fail(ctx.error, "glTF: index " + std::to_string(index) +
                                   " is out of range for a primitive with " +
                                   std::to_string(vertices) + " vertices");
      out.indices.push_back(base + index);
    }
  } else {
    // A primitive without an index accessor draws its vertices in order.
    if (vertices % 3 != 0)
      return fail(ctx.error,
                  "glTF: a primitive without indices has a vertex count that is not a "
                  "multiple of three");
    if (vertices > std::numeric_limits<u32>::max() - first)
      return fail(ctx.error, "glTF: the scene has more indices than the mesh format holds");
    out.indices.reserve(first + vertices);
    for (u32 i = 0; i < vertices; ++i)
      out.indices.push_back(base + i);
  }

  Primitive range;
  range.first_index = first;
  range.index_count = out.indices.size() - first;
  range.material = primitive.material != nullptr
                       ? static_cast<i32>(cgltf_material_index(ctx.gltf, primitive.material))
                       : -1;
  range.skin = skin_index;
  out.primitives.push_back(std::move(range));
  return true;
}

// Where glTF mesh `mesh`'s channels start in `MeshData::morph`, creating them on first sight.
// `~0u` when the mesh has no targets. Names come from `mesh.extras.targetNames` when the file
// says and are "<mesh>.<k>" when it does not; default weights come from the node's own `weights`
// if it has them and the mesh's otherwise, which is the order the glTF specification gives.
u32 begin_mesh_morph(Context& ctx, const cgltf_node& node, const cgltf_mesh& mesh) {
  if (mesh.primitives_count == 0 || mesh.primitives[0].targets_count == 0) return ~0u;
  const u32 index = static_cast<u32>(cgltf_mesh_index(ctx.gltf, &mesh));
  if (index < ctx.morph_of_mesh.size() && ctx.morph_of_mesh[index] != ~0u)
    return ctx.morph_of_mesh[index];
  const u32 base = ctx.mesh->morph.size();
  const cgltf_size targets = mesh.primitives[0].targets_count;
  const cgltf_float* weights = node.weights_count == targets
                                   ? node.weights
                                   : (mesh.weights_count == targets ? mesh.weights : nullptr);
  const std::string mesh_name = mesh.name != nullptr ? std::string(mesh.name) : std::string("mesh");
  for (cgltf_size t = 0; t < targets; ++t) {
    geometry::MorphChannelSource channel;
    channel.name = mesh.target_names != nullptr && t < mesh.target_names_count &&
                           mesh.target_names[t] != nullptr
                       ? std::string(mesh.target_names[t])
                       : mesh_name + "." + std::to_string(static_cast<u64>(t));
    channel.default_weight = weights != nullptr ? static_cast<f32>(weights[t]) : 0.0f;
    ctx.mesh->morph.push_back(std::move(channel));
  }
  if (index < ctx.morph_of_mesh.size()) ctx.morph_of_mesh[index] = base;
  return base;
}

bool append_node(Context& ctx, const cgltf_node& node, const Mat4& parent) {
  const Mat4 world = parent * local_transform(node);
  if (node.mesh != nullptr) {
    i32 skin_index = -1;
    if (node.skin != nullptr && !ctx.mesh->skins.empty())
      skin_index = static_cast<i32>(cgltf_skin_index(ctx.gltf, node.skin));
    // **A skinned primitive stays in bind space.** glTF places a skinned mesh entirely through
    // its joints, so the mesh node's own transform is not part of the vertex positions; baking
    // it in would apply the placement twice, once here and once through the joints' model
    // matrices. The transform is recorded on the skin instead, by the first node that instances
    // it, because it is otherwise the one thing about a skinned mesh this import would lose.
    const bool bind_space = skin_index >= 0;
    if (bind_space && ctx.skin_placed[static_cast<u32>(skin_index)] == 0) {
      ctx.mesh->skins[static_cast<u32>(skin_index)].node_transform = world;
      ctx.skin_placed[static_cast<u32>(skin_index)] = 1;
    }
    const Mat4 place = bind_space ? Mat4::identity() : world;
    // Normals transform by the inverse transpose, which is the only correct rule under
    // non-uniform scale; renormalized per vertex because the transform is not orthonormal.
    const Mat4 normal_transform = bind_space ? Mat4::identity() : transpose(inverse(world));
    // The morph channels this mesh owns, created the first time the mesh is reached. A mesh
    // instanced by two nodes therefore shares one set of channels and the second instance appends
    // *its own* vertices' deltas to them, which is what "channel k of this mesh" means; the two
    // instances then play the same weights, which is what the format can express and what glTF
    // node weights would have to override per instance. That override is a follow-up.
    const u32 morph_base = begin_mesh_morph(ctx, node, *node.mesh);
    for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i) {
      if (!append_primitive(ctx, node.mesh->primitives[i], place, normal_transform, skin_index,
                            morph_base))
        return false;
    }
  }
  for (cgltf_size i = 0; i < node.children_count; ++i) {
    if (node.children[i] == nullptr) continue;
    if (!append_node(ctx, *node.children[i], world)) return false;
  }
  return true;
}

bool collect_images(const cgltf_data& data, MeshData& out, std::string* error) {
  if (data.images_count > std::numeric_limits<u32>::max())
    return fail(error, "glTF: too many images");
  out.images.reserve(static_cast<u32>(data.images_count));
  for (cgltf_size i = 0; i < data.images_count; ++i) {
    const cgltf_image& image = data.images[i];
    ImageRef entry;
    if (image.name != nullptr) entry.name = image.name;
    if (image.mime_type != nullptr) entry.mime_type = image.mime_type;

    if (image.buffer_view != nullptr) {
      const u8* bytes = cgltf_buffer_view_data(image.buffer_view);
      if (bytes == nullptr)
        return fail(error, "glTF: image " + std::to_string(i) + " has no buffer data");
      if (image.buffer_view->size > std::numeric_limits<u32>::max())
        return fail(error, "glTF: image " + std::to_string(i) + " is too large");
      entry.bytes.resize(static_cast<u32>(image.buffer_view->size));
      std::memcpy(entry.bytes.data(), bytes, image.buffer_view->size);
    } else if (image.uri != nullptr && std::strncmp(image.uri, "data:", 5) == 0) {
      std::string mime;
      if (!decode_data_uri(image.uri, mime, entry.bytes))
        return fail(error,
                    "glTF: image " + std::to_string(i) + " has a data URI that is not base64");
      if (entry.mime_type.empty()) entry.mime_type = std::move(mime);
    } else if (image.uri != nullptr) {
      entry.uri = decoded_uri(image.uri);
    }
    out.images.push_back(std::move(entry));
  }
  return true;
}

bool collect_materials(const cgltf_data& data, MeshData& out, std::string* error) {
  if (data.materials_count > std::numeric_limits<u32>::max())
    return fail(error, "glTF: too many materials");
  out.materials.reserve(static_cast<u32>(data.materials_count));
  for (cgltf_size i = 0; i < data.materials_count; ++i) {
    const cgltf_material& source = data.materials[i];
    Material entry;
    if (source.name != nullptr) entry.name = source.name;
    if (source.has_pbr_metallic_roughness != 0) {
      const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
      entry.base_color = Vec4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                              pbr.base_color_factor[2], pbr.base_color_factor[3]);
      entry.metallic = pbr.metallic_factor;
      entry.roughness = pbr.roughness_factor;
      entry.base_color_image = image_slot(data, pbr.base_color_texture);
      entry.metallic_roughness_image = image_slot(data, pbr.metallic_roughness_texture);
    }
    entry.emissive =
        Vec3(source.emissive_factor[0], source.emissive_factor[1], source.emissive_factor[2]);
    entry.normal_image = image_slot(data, source.normal_texture);
    // cgltf only defaults a texture view's scale to 1 when the view is there to parse, so a
    // material with no normal texture keeps this record's own default instead of a zero.
    if (entry.normal_image >= 0) entry.normal_scale = source.normal_texture.scale;
    entry.occlusion_image = image_slot(data, source.occlusion_texture);
    entry.emissive_image = image_slot(data, source.emissive_texture);
    entry.alpha_cutoff = source.alpha_cutoff;
    entry.alpha_mode =
        source.alpha_mode == cgltf_alpha_mode_mask
            ? k_alpha_mask
            : (source.alpha_mode == cgltf_alpha_mode_blend ? k_alpha_blend : k_alpha_opaque);
    entry.double_sided = source.double_sided != 0;
    out.materials.push_back(std::move(entry));
  }
  return true;
}

// The node hierarchy, reordered so that a node's parent always comes before it. glTF puts no
// order on `nodes`, and every consumer of a skeleton — composing model transforms, retargeting,
// the pose blend — wants one forward pass rather than a recursive walk, so the sort is paid once
// here. `remap` takes a cgltf node index to its index in `MeshData::nodes`.
bool collect_nodes(const cgltf_data& data, Vector<u32>& remap, MeshData& out, std::string* error) {
  if (data.nodes_count > std::numeric_limits<u32>::max())
    return fail(error, "glTF: too many nodes");
  const u32 count = static_cast<u32>(data.nodes_count);
  Vector<u32> depth(count, 0u);
  for (u32 i = 0; i < count; ++i) {
    u32 steps = 0;
    for (const cgltf_node* up = data.nodes[i].parent; up != nullptr; up = up->parent) {
      if (++steps > count) return fail(error, "glTF: the node hierarchy has a cycle");
    }
    depth[i] = steps;
  }
  Vector<u32> order(count);
  for (u32 i = 0; i < count; ++i)
    order[i] = i;
  // Stable, so nodes at one depth keep the file's order and the import is reproducible.
  std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return depth[a] < depth[b]; });
  remap.assign(count, 0u);
  for (u32 k = 0; k < count; ++k)
    remap[order[k]] = k;

  out.nodes.reserve(count);
  for (u32 k = 0; k < count; ++k) {
    const cgltf_node& node = data.nodes[order[k]];
    Node entry;
    if (node.name != nullptr) entry.name = node.name;
    entry.local = node_trs(node);
    entry.parent =
        node.parent != nullptr
            ? static_cast<i32>(remap[static_cast<u32>(cgltf_node_index(&data, node.parent))])
            : -1;
    out.nodes.push_back(std::move(entry));
  }
  return true;
}

bool collect_skins(const cgltf_data& data, const Vector<u32>& remap, MeshData& out,
                   std::string* error) {
  out.skins.reserve(static_cast<u32>(data.skins_count));
  Vector<f32> matrices;
  for (cgltf_size i = 0; i < data.skins_count; ++i) {
    const cgltf_skin& source = data.skins[i];
    Skin entry;
    if (source.name != nullptr) entry.name = source.name;
    if (source.joints_count == 0)
      return fail(error, "glTF: skin " + std::to_string(i) + " has no joints");
    // A u8 joint index is the whole of `geometry::SkinBinding`'s palette. Refusing here is much
    // better than clamping: a wrapped index is a limb attached to the wrong bone, and the mesh
    // would look plausible until it moved.
    if (source.joints_count > geometry::k_max_skin_joints) {
      return fail(error, "glTF: skin " + std::to_string(i) + " binds " +
                             std::to_string(source.joints_count) + " joints, more than the " +
                             std::to_string(geometry::k_max_skin_joints) +
                             " a per-vertex binding indexes");
    }
    const u32 joint_count = static_cast<u32>(source.joints_count);
    entry.joints.reserve(joint_count);
    for (u32 j = 0; j < joint_count; ++j) {
      if (source.joints[j] == nullptr)
        return fail(error, "glTF: skin " + std::to_string(i) + " names a joint that is not a node");
      entry.joints.push_back(
          static_cast<i32>(remap[static_cast<u32>(cgltf_node_index(&data, source.joints[j]))]));
    }
    // glTF allows `inverseBindMatrices` to be absent, meaning the identity for every joint; the
    // array is filled in either way so a consumer never has to ask which form it got.
    entry.inverse_bind.resize(joint_count, Mat4::identity());
    if (source.inverse_bind_matrices != nullptr) {
      if (source.inverse_bind_matrices->count < source.joints_count) {
        return fail(error, "glTF: skin " + std::to_string(i) +
                               " has fewer inverse bind matrices than joints");
      }
      if (!unpack_accessor(*source.inverse_bind_matrices, 16, "inverseBindMatrices", matrices,
                           error)) {
        return false;
      }
      for (u32 j = 0; j < joint_count; ++j) {
        const f32* m = matrices.data() + static_cast<usize>(j) * 16;
        entry.inverse_bind[j] =
            Mat4(Vec4(m[0], m[1], m[2], m[3]), Vec4(m[4], m[5], m[6], m[7]),
                 Vec4(m[8], m[9], m[10], m[11]), Vec4(m[12], m[13], m[14], m[15]));
      }
    }
    if (source.skeleton != nullptr) {
      entry.skeleton_root =
          static_cast<i32>(remap[static_cast<u32>(cgltf_node_index(&data, source.skeleton))]);
    }
    out.skins.push_back(std::move(entry));
  }
  return true;
}

bool collect_animations(const cgltf_data& data, const Vector<u32>& remap,
                        const Vector<u32>& morph_of_mesh, MeshData& out, std::string* error) {
  out.animations.reserve(static_cast<u32>(data.animations_count));
  for (cgltf_size a = 0; a < data.animations_count; ++a) {
    const cgltf_animation& source = data.animations[a];
    Animation entry;
    if (source.name != nullptr) entry.name = source.name;
    entry.samplers.reserve(static_cast<u32>(source.samplers_count));
    for (cgltf_size s = 0; s < source.samplers_count; ++s) {
      const cgltf_animation_sampler& sampler = source.samplers[s];
      if (sampler.input == nullptr || sampler.output == nullptr)
        return fail(error, "glTF: an animation sampler has no input or output accessor");
      AnimationSampler curve;
      curve.interpolation =
          sampler.interpolation == cgltf_interpolation_type_step
              ? k_interp_step
              : (sampler.interpolation == cgltf_interpolation_type_cubic_spline ? k_interp_cubic
                                                                                : k_interp_linear);
      if (!unpack_accessor(*sampler.input, 1, "an animation sampler's times", curve.times, error))
        return false;
      const cgltf_size components = cgltf_num_components(sampler.output->type);
      if (components != 1 && components != 3 && components != 4) {
        return fail(error,
                    "glTF: an animation sampler's values are neither scalar nor three nor "
                    "four components");
      }
      if (!unpack_accessor(*sampler.output, static_cast<u32>(components),
                           "an animation sampler's values", curve.values, error)) {
        return false;
      }
      // CUBICSPLINE stores in-tangent, value, and out-tangent per key, so three output elements
      // per input time; the other two store one.
      const cgltf_size per_key = curve.interpolation == k_interp_cubic ? 3 : 1;
      // A **weights** sampler is SCALAR with one value per morph target per key, so its output
      // count is the target count times the key count — which is how the target count is
      // recovered, since the accessor itself does not say. A TRS sampler has exactly one element
      // per key, so the same division gives 1 and the check below is what it always was.
      cgltf_size per_element = 1;
      if (components == 1 && sampler.input->count * per_key != 0) {
        const cgltf_size denominator = sampler.input->count * per_key;
        if (sampler.output->count % denominator != 0 || sampler.output->count == 0) {
          return fail(error, "glTF: a scalar animation sampler has " +
                                 std::to_string(sampler.output->count) + " values for " +
                                 std::to_string(sampler.input->count) +
                                 " keyframe times, which is not a whole number of targets");
        }
        per_element = sampler.output->count / denominator;
        if (per_element > 255) {
          return fail(error, "glTF: an animation drives " + std::to_string(per_element) +
                                 " morph targets, more than the 255 a track holds");
        }
      }
      curve.components = static_cast<u8>(components == 1 ? per_element : components);
      if (sampler.output->count != sampler.input->count * per_key * per_element) {
        return fail(error, "glTF: an animation sampler has " +
                               std::to_string(sampler.output->count) + " values for " +
                               std::to_string(sampler.input->count) + " keyframe times");
      }
      if (!curve.times.empty() && curve.times[curve.times.size() - 1] > entry.duration)
        entry.duration = curve.times[curve.times.size() - 1];
      entry.samplers.push_back(std::move(curve));
    }
    entry.channels.reserve(static_cast<u32>(source.channels_count));
    for (cgltf_size c = 0; c < source.channels_count; ++c) {
      const cgltf_animation_channel& channel = source.channels[c];
      if (channel.target_node == nullptr || channel.sampler == nullptr) continue;
      u8 path = k_path_translation;
      if (channel.target_path == cgltf_animation_path_type_rotation) {
        path = k_path_rotation;
      } else if (channel.target_path == cgltf_animation_path_type_scale) {
        path = k_path_scale;
      } else if (channel.target_path == cgltf_animation_path_type_weights) {
        path = k_path_weights;
      } else if (channel.target_path != cgltf_animation_path_type_translation) {
        continue;
      }
      AnimationChannel entry_channel;
      entry_channel.node =
          static_cast<i32>(remap[static_cast<u32>(cgltf_node_index(&data, channel.target_node))]);
      entry_channel.sampler = static_cast<u32>(channel.sampler - source.samplers);
      entry_channel.path = path;
      if (path == k_path_weights) {
        // Which of the flattened channel array the curve's first value per key drives. The
        // scene walk fills `morph_of_mesh`, so this runs after it; a channel whose mesh the
        // default scene never instanced has no channels here and is dropped rather than left
        // pointing at another mesh's rig.
        if (channel.target_node->mesh == nullptr) continue;
        const u32 mesh_index = static_cast<u32>(cgltf_mesh_index(&data, channel.target_node->mesh));
        if (mesh_index >= morph_of_mesh.size() || morph_of_mesh[mesh_index] == ~0u) continue;
        entry_channel.morph_channel = morph_of_mesh[mesh_index];
      }
      entry.channels.push_back(entry_channel);
    }
    out.animations.push_back(std::move(entry));
  }
  return true;
}

bool build_mesh(const cgltf_data& data, MeshData& out, std::string* error) {
  if (!collect_images(data, out, error)) return false;
  if (!collect_materials(data, out, error)) return false;

  // The hierarchy, the skins, and the animation curves, only for a file that has one of the last
  // two: a static mesh imports exactly as it always did, with none of these arrays allocated.
  Vector<u32> node_remap;
  const bool rigged = data.skins_count > 0 || data.animations_count > 0;
  if (rigged) {
    if (!collect_nodes(data, node_remap, out, error)) return false;
    if (!collect_skins(data, node_remap, out, error)) return false;
  }

  const cgltf_scene* scene = data.scene;
  if (scene == nullptr && data.scenes_count > 0) scene = &data.scenes[0];
  if (scene == nullptr) return fail(error, "glTF: the file has no scene to traverse");

  Context ctx;
  ctx.gltf = &data;
  ctx.mesh = &out;
  ctx.error = error;
  ctx.has_skins = !out.skins.empty();
  ctx.node_remap = node_remap;
  ctx.skin_placed.resize(out.skins.size(), u8{0});
  ctx.morph_of_mesh.resize(static_cast<u32>(data.meshes_count), ~0u);
  for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
    if (scene->nodes[i] == nullptr) continue;
    if (!append_node(ctx, *scene->nodes[i], Mat4::identity())) return false;
  }
  if (out.positions.empty()) return fail(error, "glTF: the scene has no triangle geometry");
  if (!ctx.saw_normals) out.normals.clear();
  if (!ctx.saw_uvs) out.uvs.clear();
  // A file whose skins the default scene never instances imports as a static mesh: the stream
  // exists only when something in the picture is actually skinned.
  if (!ctx.saw_skin) out.skin_bindings.clear();
  // A channel every primitive of its mesh left empty moves nothing; it stays in the array rather
  // than being removed, because the weight tracks index it by position.
  //
  // The animations come **after** the walk, and only because of the weight tracks: a `weights`
  // channel has to be told which entry of the flattened channel array it drives, and that is
  // `morph_of_mesh`, which only the walk knows. Nothing else about them changed.
  if (rigged && !collect_animations(data, node_remap, ctx.morph_of_mesh, out, error)) return false;
  return true;
}

}  // namespace

bool load_gltf_memory(std::span<const u8> bytes, std::string_view base_dir, MeshData& out,
                      std::string* error) {
  out = MeshData{};
  if (bytes.empty()) return fail(error, "glTF: the input is empty");

  cgltf_options options{};
  options.file.read = &read_through_io;
  options.file.release = &release_through_io;

  Document document;
  cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &document.data);
  if (result != cgltf_result_success)
    return fail(error, std::string("glTF: parsing failed (") + result_text(result) + ")");

  // cgltf resolves a buffer URI against the directory of the path it is given, so a name inside
  // base_dir stands in for the file itself; an empty base_dir leaves URIs relative to the
  // working directory, which is what a caller with no directory can offer.
  const std::string root = base_dir.empty() ? std::string("gltf") : io::join_path(base_dir, "gltf");
  result = cgltf_load_buffers(&options, document.data, root.c_str());
  if (result != cgltf_result_success)
    return fail(error, std::string("glTF: reading buffers failed (") + result_text(result) + ")");

  result = cgltf_validate(document.data);
  if (result != cgltf_result_success)
    return fail(error,
                std::string("glTF: the document is not valid (") + result_text(result) + ")");

  if (!build_mesh(*document.data, out, error)) {
    out = MeshData{};
    return false;
  }
  return true;
}

bool load_gltf(std::string_view path, MeshData& out, std::string* error) {
  out = MeshData{};
  std::string contents;
  const io::Status status = io::read_file(path, contents);
  if (status != io::Status::Ok)
    return fail(error, std::string("glTF: cannot read '") + std::string(path) +
                           "': " + io::status_name(status));
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(contents.data()), contents.size());
  return load_gltf_memory(bytes, io::parent_path(path), out, error);
}

bool source_mesh_hash(std::string_view path, u64& out, std::string* error) {
  out = 0;
  std::string contents;
  const io::Status status = io::read_file(path, contents);
  if (status != io::Status::Ok)
    return fail(error, std::string("glTF: cannot read '") + std::string(path) +
                           "': " + io::status_name(status));
  u64 hash = hash_bytes(contents.data(), contents.size());

  // Only the parse, not cgltf_load_buffers: the buffer URIs are all that is wanted here, and a
  // file whose buffers are missing still gets a hash that changes when the glTF changes.
  cgltf_options options{};
  options.file.read = &read_through_io;
  options.file.release = &release_through_io;
  Document document;
  const cgltf_result result =
      cgltf_parse(&options, contents.data(), contents.size(), &document.data);
  if (result != cgltf_result_success)
    return fail(error, std::string("glTF: parsing failed (") + result_text(result) + ")");

  const std::string base_dir(io::parent_path(path));
  for (cgltf_size i = 0; i < document.data->buffers_count; ++i) {
    const char* uri = document.data->buffers[i].uri;
    if (uri == nullptr || std::strncmp(uri, "data:", 5) == 0) continue;
    const std::string relative = decoded_uri(uri);
    const std::string file = base_dir.empty() ? relative : io::join_path(base_dir, relative);
    std::string buffer;
    if (io::read_file(file, buffer) != io::Status::Ok) buffer.clear();
    hash = hash_combine(hash, hash_bytes(buffer.data(), buffer.size()));
  }
  out = hash;
  return true;
}

geometry::AttributeSource attribute_source(const MeshData& mesh) noexcept {
  geometry::AttributeSource source;
  if (!mesh.normals.empty())
    source.normals = std::span<const Vec3>(mesh.normals.data(), mesh.normals.size());
  if (!mesh.uvs.empty()) source.uvs = std::span<const Vec2>(mesh.uvs.data(), mesh.uvs.size());
  if (!mesh.morph.empty())
    source.morph =
        std::span<const geometry::MorphChannelSource>(mesh.morph.data(), mesh.morph.size());
  if (mesh.skin_bindings.size() == mesh.positions.size() && !mesh.skin_bindings.empty()) {
    source.skin = std::span<const geometry::SkinBinding>(mesh.skin_bindings.data(),
                                                         mesh.skin_bindings.size());
    // A vertex's joint indices are its own primitive's skin's; the widest skin of the file is
    // therefore the bound every index in the merged stream satisfies, which is exactly what
    // `validate_clusters` needs. A mesh built per primitive (what the content build does) keeps
    // the skins apart anyway.
    for (const Skin& skin : mesh.skins) {
      if (skin.joints.size() > source.joint_count) source.joint_count = skin.joints.size();
    }
  }
  return source;
}

bool repair_uv_degenerate_triangles(MeshData& mesh, geometry::UvRepairReport& report,
                                    std::string* error) {
  // Runs in index order, which is the order the flatten appends primitives in; `order` maps each
  // run back to its primitive so a file whose primitives were listed some other way still gets
  // its own ranges back.
  Vector<u32> order(mesh.primitives.size());
  for (u32 p = 0; p < order.size(); ++p)
    order[p] = p;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return mesh.primitives[a].first_index != mesh.primitives[b].first_index
               ? mesh.primitives[a].first_index < mesh.primitives[b].first_index
               : a < b;
  });
  Vector<geometry::UvRepairRange> ranges;
  ranges.reserve(order.size());
  for (const u32 p : order) {
    const Primitive& primitive = mesh.primitives[p];
    // Only a material that samples a texture cares what its UVs are; a primitive with no material
    // or an untextured one is left exactly as the file had it.
    bool textured = false;
    if (primitive.material >= 0 && static_cast<u32>(primitive.material) < mesh.materials.size()) {
      const Material& m = mesh.materials[static_cast<u32>(primitive.material)];
      textured = m.base_color_image >= 0 || m.metallic_roughness_image >= 0 ||
                 m.normal_image >= 0 || m.occlusion_image >= 0 || m.emissive_image >= 0;
    }
    ranges.push_back(
        geometry::UvRepairRange{primitive.first_index, primitive.index_count, textured});
  }
  if (!geometry::repair_uv_degenerate_triangles(
          std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()), mesh.normals,
          mesh.uvs, mesh.indices, std::span<geometry::UvRepairRange>(ranges.data(), ranges.size()),
          geometry::UvRepairOptions{}, report, error)) {
    return false;
  }
  for (u32 r = 0; r < order.size(); ++r) {
    mesh.primitives[order[r]].first_index = ranges[r].first;
    mesh.primitives[order[r]].index_count = ranges[r].count;
  }
  return true;
}

u32 weld_vertices(MeshData& mesh) {
  // Every per-vertex stream `MeshData` owns, and nothing is optional here: the weld takes part in
  // a stream only when it is parallel to the positions (skin) or non-empty (morph), so passing an
  // absent one costs nothing, while leaving a present one out would leave it in the old numbering.
  return geometry::weld_vertices(mesh.positions, mesh.normals, mesh.uvs,
                                 std::span<u32>(mesh.indices.data(), mesh.indices.size()),
                                 &mesh.skin_bindings, &mesh.morph);
}

}  // namespace engine::assets
